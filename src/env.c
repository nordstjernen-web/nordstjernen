/* Nordstjernen — runtime environment info shared by the JS console and about: page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "env.h"

#ifndef __ANDROID__
#include <gtk/gtk.h>
#endif

#include "css.h"
#include "js.h"
#include "net.h"
#include "quickjs.h"

#ifndef G_OS_WIN32
#include <sys/utsname.h>
#endif

static const char *
nd_os_name(void)
{
#ifdef G_OS_WIN32
    return "Windows";
#elif defined(__ANDROID__)
    return "Android";
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

void
nd_env_each(nd_env_emit_fn emit, gpointer user_data)
{
    char buf[160];

    const char *hv = nd_html_engine_version();
    if (hv && *hv) {
        g_snprintf(buf, sizeof(buf), "%s %s",
                   nd_html_engine_name(), hv);
        emit("HTML parser", buf, user_data);
    } else {
        emit("HTML parser", nd_html_engine_name(), user_data);
    }

    emit("CSS engine", "Nordstjernen", user_data);

    g_snprintf(buf, sizeof(buf), "QuickJS-ng %d.%d.%d%s",
               QJS_VERSION_MAJOR, QJS_VERSION_MINOR, QJS_VERSION_PATCH,
               QJS_VERSION_SUFFIX);
    emit("JS engine", buf, user_data);

    emit("JS active",  nd_js_engine_name(), user_data);

#ifndef __ANDROID__
    g_snprintf(buf, sizeof(buf), "%u.%u.%u",
               gtk_get_major_version(),
               gtk_get_minor_version(),
               gtk_get_micro_version());
    emit("GTK", buf, user_data);
#endif

    emit("OS", nd_os_name(), user_data);

    {
        char *proxy = nd_net_effective_proxy_for("https://example.com/");
        emit("Proxy (https)", proxy && *proxy ? proxy : "(direct)", user_data);
        g_free(proxy);
    }
    {
        char *proxy = nd_net_effective_proxy_for("http://example.com/");
        emit("Proxy (http)",  proxy && *proxy ? proxy : "(direct)", user_data);
        g_free(proxy);
    }
}
