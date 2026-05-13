/* Nordstjernen — runtime environment info shared by the JS console and about: page. */

#include "env.h"

#include <gtk/gtk.h>

#include <lexbor/core/base.h>

#include "css.h"
#include "quickjs.h"

#ifndef G_OS_WIN32
#include <sys/utsname.h>
#endif

const char *
nd_os_name(void)
{
#ifdef G_OS_WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    static char buf[128];
    if (!buf[0]) {
        struct utsname u;
        if (uname(&u) == 0)
            g_snprintf(buf, sizeof(buf), "%s %s (%s)",
                       u.sysname, u.release, u.machine);
        else
            g_snprintf(buf, sizeof(buf), "Linux");
    }
    return buf;
#endif
}

const char *
nd_html_engine_version(nd_html_engine engine)
{
    switch (engine) {
    case ND_HTML_ENGINE_LEXBOR:
#ifdef ND_LEXBOR_VERSION
        return ND_LEXBOR_VERSION;
#else
        return LEXBOR_VERSION_STRING;
#endif
    case ND_HTML_ENGINE_GUMBO:
    default:
        return NULL;
    }
}

void
nd_env_each(nd_env_emit_fn emit, gpointer user_data)
{
    char buf[160];

    nd_html_engine he = nd_html_engine_default();
    const char *hv = nd_html_engine_version(he);
    if (hv) {
        g_snprintf(buf, sizeof(buf), "%s %s",
                   nd_html_engine_name(he), hv);
        emit("HTML parser", buf, user_data);
    } else {
        emit("HTML parser", nd_html_engine_name(he), user_data);
    }

    emit("CSS engine",
         nd_css_engine_name(nd_css_engine_default()),
         user_data);

    g_snprintf(buf, sizeof(buf), "QuickJS-ng %d.%d.%d%s",
               QJS_VERSION_MAJOR, QJS_VERSION_MINOR, QJS_VERSION_PATCH,
               QJS_VERSION_SUFFIX);
    emit("JS engine", buf, user_data);

    g_snprintf(buf, sizeof(buf), "%u.%u.%u",
               gtk_get_major_version(),
               gtk_get_minor_version(),
               gtk_get_micro_version());
    emit("GTK", buf, user_data);

    emit("OS", nd_os_name(), user_data);
}
