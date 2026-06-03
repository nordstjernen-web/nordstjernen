/* Nordstjernen — JavaScript bytecode cache (in-memory + on-disk). */

#ifndef ND_BCACHE_H
#define ND_BCACHE_H

#include <glib.h>

G_BEGIN_DECLS

void          nd_bcache_init(void);
void          nd_bcache_shutdown(void);

guint8 *nd_bcache_get(const char *src, gsize src_len, gsize *out_len);
void    nd_bcache_put(const char *src, gsize src_len,
                      const guint8 *bc, gsize bc_len);

G_END_DECLS

#endif
