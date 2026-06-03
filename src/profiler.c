/* Nordstjernen — gdb-driven sampling profiler for the JS console.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "profiler.h"

#include <gio/gio.h>
#include <string.h>

#ifdef G_OS_UNIX
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

typedef struct nd_profile_job {
    GSubprocess           *child;
    GDataInputStream      *stdout_stream;
    guint                  samples_requested;
    guint                  samples_taken;
    guint                  thread_snapshots;
    guint                  interval_ms;
    gint64                 t_start_us;
    GHashTable            *top_counts;
    GHashTable            *leaf_counts;
    char                  *current_top_fn;
    char                  *current_leaf_fn;
    gboolean               saw_any_frame;
    nd_profile_progress_fn progress;
    nd_profile_done_fn     done;
    gpointer               user_data;
    char                  *error_message;
} nd_profile_job;

gboolean
nd_profiler_supported(void)
{
#ifdef __linux__
    char *gdb = g_find_program_in_path("gdb");
    if (gdb) { g_free(gdb); return TRUE; }
#endif
    return FALSE;
}

static void
nd_profile_result_free(nd_profile_result *r)
{
    if (!r) return;
    g_free(r->error_message);
    if (r->top_rows) {
        for (guint i = 0; i < r->top_rows->len; i++)
            g_free(g_array_index(r->top_rows, nd_profile_row, i).function);
        g_array_free(r->top_rows, TRUE);
    }
    if (r->leaf_rows) {
        for (guint i = 0; i < r->leaf_rows->len; i++)
            g_free(g_array_index(r->leaf_rows, nd_profile_row, i).function);
        g_array_free(r->leaf_rows, TRUE);
    }
    g_free(r);
}

static int
nd_profile_row_cmp_desc(gconstpointer a, gconstpointer b)
{
    const nd_profile_row *ra = a;
    const nd_profile_row *rb = b;
    if (ra->hits != rb->hits) return (int)rb->hits - (int)ra->hits;
    return g_strcmp0(ra->function, rb->function);
}

static GArray *
nd_profile_rows_from_counts(GHashTable *counts)
{
    GArray *rows = g_array_new(FALSE, FALSE, sizeof(nd_profile_row));
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, counts);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        nd_profile_row r = { g_strdup(k), GPOINTER_TO_UINT(v) };
        g_array_append_val(rows, r);
    }
    g_array_sort(rows, nd_profile_row_cmp_desc);
    return rows;
}

static void
nd_profile_job_free(nd_profile_job *j)
{
    if (!j) return;
    g_clear_object(&j->stdout_stream);
    g_clear_object(&j->child);
    if (j->top_counts)  g_hash_table_destroy(j->top_counts);
    if (j->leaf_counts) g_hash_table_destroy(j->leaf_counts);
    g_free(j->current_top_fn);
    g_free(j->current_leaf_fn);
    g_free(j->error_message);
    g_free(j);
}

static void
nd_profile_emit_done(nd_profile_job *j)
{
#if defined(__linux__) && defined(PR_SET_PTRACER)
    prctl(PR_SET_PTRACER, 0, 0, 0, 0);
#endif
    nd_profile_result *r = g_new0(nd_profile_result, 1);
    r->samples_requested = j->samples_requested;
    r->samples_taken     = j->samples_taken;
    r->thread_snapshots  = j->thread_snapshots;
    r->interval_ms       = j->interval_ms;
    r->wall_us           = g_get_monotonic_time() - j->t_start_us;
    r->ok                = (j->error_message == NULL) && (j->samples_taken > 0);
    r->error_message     = j->error_message;
    j->error_message     = NULL;
    r->top_rows  = nd_profile_rows_from_counts(j->top_counts);
    r->leaf_rows = nd_profile_rows_from_counts(j->leaf_counts);

    if (j->done) j->done(r, j->user_data);

    nd_profile_result_free(r);
    nd_profile_job_free(j);
}

static char *
nd_profile_extract_function(const char *line)
{
    const char *in = strstr(line, " in ");
    if (!in) return NULL;
    const char *name_start = in + 4;
    while (*name_start == ' ') name_start++;
    if (!*name_start) return NULL;
    const char *p = name_start;
    while (*p && *p != ' ' && *p != '(' && *p != '\n') p++;
    if (p == name_start) return NULL;
    return g_strndup(name_start, (gsize)(p - name_start));
}

static gboolean
nd_profile_line_is_frame(const char *line, guint *out_index)
{
    if (line[0] != '#') return FALSE;
    const char *p = line + 1;
    guint idx = 0;
    if (!g_ascii_isdigit(*p)) return FALSE;
    while (g_ascii_isdigit(*p)) {
        idx = idx * 10 + (guint)(*p - '0');
        p++;
    }
    if (*p != ' ') return FALSE;
    *out_index = idx;
    return TRUE;
}

static void
nd_profile_finalise_thread(nd_profile_job *j)
{
    if (!j->saw_any_frame) return;
    if (j->current_top_fn) {
        gpointer cur = g_hash_table_lookup(j->top_counts, j->current_top_fn);
        guint nv = GPOINTER_TO_UINT(cur) + 1;
        g_hash_table_replace(j->top_counts, g_strdup(j->current_top_fn),
                             GUINT_TO_POINTER(nv));
    }
    if (j->current_leaf_fn) {
        gpointer cur = g_hash_table_lookup(j->leaf_counts, j->current_leaf_fn);
        guint nv = GPOINTER_TO_UINT(cur) + 1;
        g_hash_table_replace(j->leaf_counts, g_strdup(j->current_leaf_fn),
                             GUINT_TO_POINTER(nv));
    }
    j->thread_snapshots++;
    g_clear_pointer(&j->current_top_fn,  g_free);
    g_clear_pointer(&j->current_leaf_fn, g_free);
    j->saw_any_frame = FALSE;
}

static void
nd_profile_finalise_sample(nd_profile_job *j)
{
    nd_profile_finalise_thread(j);
    j->samples_taken++;
    if (j->progress) j->progress(j->samples_taken, j->samples_requested, j->user_data);
}

static gboolean
nd_profile_is_uninteresting_frame(const char *fn)
{
    if (!fn) return TRUE;
    if (fn[0] == '?' || fn[0] == '<') return TRUE;
    static const char *skip_prefix[] = {
        "__GI___poll", "__GI___libc_read", "__GI___libc_recv",
        "__GI___libc_send", "__GI___nanosleep", "__GI___clock_nanosleep",
        "__pthread_cond_wait", "___pthread_cond_wait",
        "__futex_abstimed_wait", "__futex_wait",
        "__GI___select", "__select",
        "g_main_loop_run", "g_main_context_iteration",
        "g_main_context_dispatch", "g_main_context_poll",
        "g_main_context_iterate", "g_main_context_check",
        "g_main_dispatch",
        "g_cond_wait", "g_async_queue_pop", "g_async_queue_timed_pop",
        "pthread_cond_wait", "pthread_cond_timedwait",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(skip_prefix); i++)
        if (g_str_has_prefix(fn, skip_prefix[i])) return TRUE;
    static const char *skip_exact[] = {
        "syscall", "poll", "ppoll", "epoll_wait", "select",
        "read", "recv", "recvfrom", "recvmsg",
        "__poll", "__libc_pause",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(skip_exact); i++)
        if (strcmp(fn, skip_exact[i]) == 0) return TRUE;
    return FALSE;
}

static void
nd_profile_ingest_line(nd_profile_job *j, const char *line)
{
    if (!line) return;
    if (strstr(line, "===SAMPLE-END===")) {
        nd_profile_finalise_sample(j);
        return;
    }
    const char *err_marker = strstr(line, "===PROFILER-ERROR===");
    if (err_marker) {
        const char *msg = err_marker + strlen("===PROFILER-ERROR===");
        while (*msg == ' ') msg++;
        g_free(j->error_message);
        j->error_message = g_strdup(msg);
        return;
    }
    if (g_str_has_prefix(line, "Thread ") && strstr(line, "(LWP ")) {
        nd_profile_finalise_thread(j);
        return;
    }
    guint idx;
    if (!nd_profile_line_is_frame(line, &idx)) return;
    char *fn = nd_profile_extract_function(line);
    if (!fn) return;
    if (idx == 0) {
        g_free(j->current_top_fn);
        j->current_top_fn = g_strdup(fn);
    }
    if (!nd_profile_is_uninteresting_frame(fn)) {
        g_free(j->current_leaf_fn);
        j->current_leaf_fn = g_strdup(fn);
    }
    j->saw_any_frame = TRUE;
    g_free(fn);
}

static void nd_profile_read_next_line(nd_profile_job *j);

static void
nd_profile_on_line(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_profile_job *j = user_data;
    GError *err = NULL;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish_utf8(
        G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (!line) {
        if (err) {
            if (!j->error_message)
                j->error_message = g_strdup_printf("read: %s", err->message);
            g_error_free(err);
        }
        if (j->current_top_fn || j->current_leaf_fn)
            nd_profile_finalise_sample(j);
        nd_profile_emit_done(j);
        return;
    }
    nd_profile_ingest_line(j, line);
    g_free(line);
    nd_profile_read_next_line(j);
}

static void
nd_profile_read_next_line(nd_profile_job *j)
{
    g_data_input_stream_read_line_async(j->stdout_stream, G_PRIORITY_DEFAULT,
                                        NULL, nd_profile_on_line, j);
}

#ifdef __linux__
static void
nd_profile_make_traceable(void)
{
#ifdef PR_SET_PTRACER
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
}
#endif

gboolean
nd_profiler_run_async(guint samples, guint interval_ms,
                      nd_profile_progress_fn progress,
                      nd_profile_done_fn done,
                      gpointer user_data)
{
    if (samples == 0) samples = 30;
    if (interval_ms == 0) interval_ms = 50;
    if (samples > 500) samples = 500;
    if (interval_ms < 5) interval_ms = 5;

    if (!nd_profiler_supported()) {
        if (done) {
            nd_profile_result r = {0};
            r.samples_requested = samples;
            r.interval_ms       = interval_ms;
            r.error_message     = g_strdup("gdb not found in PATH");
            r.top_rows  = g_array_new(FALSE, FALSE, sizeof(nd_profile_row));
            r.leaf_rows = g_array_new(FALSE, FALSE, sizeof(nd_profile_row));
            done(&r, user_data);
            g_free(r.error_message);
            g_array_free(r.top_rows, TRUE);
            g_array_free(r.leaf_rows, TRUE);
        }
        return FALSE;
    }

#ifdef __linux__
    nd_profile_make_traceable();
    pid_t pid = getpid();
#else
    int pid = 0;
#endif
    GString *script = g_string_new(NULL);
    g_string_append_printf(script,
        "set -e\n"
        "PID=%d\n"
        "SAMPLES=%u\n"
        "INTERVAL_MS=%u\n"
        "SLEEP_S=$(awk -v ms=$INTERVAL_MS 'BEGIN{printf \"%%.3f\", ms/1000.0}')\n"
        "for i in $(seq 1 $SAMPLES); do\n"
        "  if ! gdb -batch -p $PID \\\n"
        "      -ex 'set pagination off' \\\n"
        "      -ex 'set print frame-arguments none' \\\n"
        "      -ex 'thread apply all -- bt 16' 2>/dev/null; then\n"
        "    echo '===PROFILER-ERROR=== gdb attach failed (ptrace blocked?)'\n"
        "    exit 1\n"
        "  fi\n"
        "  echo '===SAMPLE-END==='\n"
        "  sleep $SLEEP_S\n"
        "done\n",
        (int)pid, samples, interval_ms);

    GError *err = NULL;
    const char *argv[] = { "bash", "-c", script->str, NULL };
    GSubprocess *child = g_subprocess_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &err);
    g_string_free(script, TRUE);
    if (!child) {
        if (done) {
            nd_profile_result r = {0};
            r.samples_requested = samples;
            r.interval_ms       = interval_ms;
            r.error_message = g_strdup_printf("spawn failed: %s",
                                              err ? err->message : "?");
            r.top_rows  = g_array_new(FALSE, FALSE, sizeof(nd_profile_row));
            r.leaf_rows = g_array_new(FALSE, FALSE, sizeof(nd_profile_row));
            done(&r, user_data);
            g_free(r.error_message);
            g_array_free(r.top_rows, TRUE);
            g_array_free(r.leaf_rows, TRUE);
        }
        if (err) g_error_free(err);
        return FALSE;
    }

    nd_profile_job *j = g_new0(nd_profile_job, 1);
    j->child = child;
    j->stdout_stream = g_data_input_stream_new(
        g_subprocess_get_stdout_pipe(child));
    j->samples_requested = samples;
    j->interval_ms       = interval_ms;
    j->t_start_us        = g_get_monotonic_time();
    j->top_counts  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    j->leaf_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    j->progress  = progress;
    j->done      = done;
    j->user_data = user_data;

    nd_profile_read_next_line(j);
    return TRUE;
}
