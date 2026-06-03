/* Nordstjernen — HTML helper utilities shared across the lexbor frontend.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "html.h"

#include <string.h>
#include <uchardet.h>

#include <lexbor/core/base.h>

gboolean
nd_html_is_void(const char *tag)
{
    if (!tag) return FALSE;
    static const char *const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
        NULL,
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(tag, voids[i]) == 0)
            return TRUE;
    return FALSE;
}

void
nd_html_escape_append(GString *out, const char *s, gboolean escape_quotes)
{
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '&': g_string_append(out, "&amp;"); break;
        case '<': g_string_append(out, "&lt;");  break;
        case '>': g_string_append(out, "&gt;");  break;
        case '"':
            if (escape_quotes) g_string_append(out, "&quot;");
            else               g_string_append_c(out, '"');
            break;
        default:  g_string_append_c(out, *p);    break;
        }
    }
}

char *
nd_html_escape_text(const char *s)
{
    GString *g = g_string_new(NULL);
    nd_html_escape_append(g, s, TRUE);
    return g_string_free(g, FALSE);
}

const char *
nd_html_engine_name(void)
{
    return "lexbor";
}

const char *
nd_html_engine_version(void)
{
#ifdef ND_LEXBOR_VERSION
    return ND_LEXBOR_VERSION;
#else
    return LEXBOR_VERSION_STRING;
#endif
}

char *
nd_html_decode_body(const char *body, gsize len)
{
    if (!body || len == 0) return g_strdup("");

    if (g_utf8_validate(body, (gssize)len, NULL))
        return g_strndup(body, len);

    char *charset = NULL;
    uchardet_t det = uchardet_new();
    if (det) {
        gsize scan = len < 65536 ? len : 65536;
        if (uchardet_handle_data(det, body, scan) == 0) {
            uchardet_data_end(det);
            const char *name = uchardet_get_charset(det);
            if (name && *name
                && g_ascii_strcasecmp(name, "ASCII") != 0
                && g_ascii_strcasecmp(name, "UTF-8") != 0)
                charset = g_strdup(name);
        }
        uchardet_delete(det);
    }

    if (charset) {
        char *out = g_convert(body, (gssize)len, "UTF-8", charset,
                              NULL, NULL, NULL);
        g_free(charset);
        if (out) return out;
    }

    char *latin1 = g_convert(body, (gssize)len, "UTF-8", "ISO-8859-1",
                             NULL, NULL, NULL);
    if (latin1) return latin1;

    return g_utf8_make_valid(body, (gssize)len);
}
