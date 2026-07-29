/* Nordstjernen — the text layer the engine talks to.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_PANGO_H
#define NS_PANGO_H

/* Desktop builds shape text through ns-pango, a Pango fork that caches
 * finished glyph strings across layouts. Android and iOS keep the system
 * Pango: the fork carries no CoreText backend and requires fontconfig.
 * ns_pango_names.h maps the fork's renamed API back to stock Pango so the
 * engine sources compile unchanged against either.
 */

#ifdef NS_USE_NS_PANGO
#include <ns-pango/pangocairo.h>
#ifdef NS_HAVE_PANGOFT2
#include <ns-pango/pangofc-fontmap.h>
#endif
#else
#include <pango/pangocairo.h>
#ifdef NS_HAVE_PANGOFT2
#include <pango/pangofc-fontmap.h>
#endif
#include "ns_pango_names.h"
#endif

#endif /* NS_PANGO_H */
