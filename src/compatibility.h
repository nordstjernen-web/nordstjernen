/* Nordstjernen — per-site compatibility framework.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_COMPATIBILITY_H
#define ND_COMPATIBILITY_H

#include <glib.h>

#include "css.h"
#include "dom.h"
#include "html.h"

G_BEGIN_DECLS

const char *nd_compat_user_agent_for_host(const char *host);

const char *nd_compat_user_agent_for_url(const char *url);

nd_css_stylesheet *nd_compat_stylesheet_for_host(const char *host);

nd_css_stylesheet *nd_compat_stylesheet_for_url(const char *url);

void nd_compat_rewrite_doc(nd_node *root, const char *page_url);

char *nd_google_unwrap_consent_url(const char *url);

char *nd_google_rewrite_url(const char *url);

G_END_DECLS

#endif
