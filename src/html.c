/* Nordstjernen — HTML parser dispatcher (lexbor).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "html.h"

#include <stdlib.h>
#include <string.h>
#include <uchardet/uchardet.h>

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

nd_node *
nd_html_parse(const char *input, gssize len)
{
    return nd_html_parse_lexbor(input, len);
}

nd_node *
nd_html_parse_for_url(const char *url, const char *input, gssize len)
{
    (void)url;
    return nd_html_parse_lexbor(input, len);
}

nd_node *
nd_html_parse_for_page(const char *input, gssize len)
{
    return nd_html_parse_lexbor(input, len);
}

nd_node *
nd_html_parse_fragment_in(const char *context_tag,
                          const char *input, gssize len)
{
    return nd_html_parse_fragment_lexbor(context_tag, input, len);
}

nd_node *
nd_html_parse_fragment(const char *input, gssize len)
{
    return nd_html_parse_fragment_lexbor(NULL, input, len);
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
