/* Nordstjernen — IndexedDB backend bridge.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_IDB_H
#define ND_IDB_H

#include <quickjs.h>

void nd_idb_install(JSContext *ctx, JSValueConst global);

#endif
