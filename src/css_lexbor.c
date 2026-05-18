/* Nordstjernen — CSS engine: lexbor backend (delegates to ours until filled in).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "css.h"

nd_css_stylesheet *
nd_css_stylesheet_parse_lexbor(const char *text, gssize len)
{
    return nd_css_stylesheet_parse_ours(text, len);
}

GPtrArray *
nd_css_parse_selector_list_lexbor(const char *text)
{
    return nd_css_parse_selector_list_ours(text);
}

gboolean
nd_css_selector_matches_lexbor(const nd_css_selector *sel, const nd_node *el)
{
    return nd_css_selector_matches_ours(sel, el);
}

gboolean
nd_css_media_query_matches_lexbor(const char *query)
{
    return nd_css_media_query_matches_ours(query);
}
