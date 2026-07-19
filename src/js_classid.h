/* Nordstjernen — process-global JS class-ID allocation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_JS_CLASSID_H
#define NS_JS_CLASSID_H

#include <quickjs.h>

JSClassID ns_new_class_id(JSClassID *pclass_id);

#endif
