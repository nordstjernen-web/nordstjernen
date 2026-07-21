/* Nordstjernen — inline MPEG-1 video decoding (pl_mpeg).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_VIDEO_DECODE_H
#define NS_VIDEO_DECODE_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_video_player ns_video_player;

ns_video_player *ns_video_player_new(const guint8 *bytes, gsize len);
void             ns_video_player_free(ns_video_player *player);

gboolean ns_video_player_has_audio(const ns_video_player *player);
int    ns_video_player_width(const ns_video_player *player);
int    ns_video_player_height(const ns_video_player *player);
double ns_video_player_duration(const ns_video_player *player);
double ns_video_player_buffered_end(const ns_video_player *player);
void   ns_video_player_note_end(ns_video_player *player, double end);
gboolean ns_video_probe_chunk_range(const guint8 *init, gsize init_len,
                                    const guint8 *chunk, gsize chunk_len,
                                    double *out_start, double *out_end);
gboolean ns_video_codec_available(const char *codec);
gboolean ns_video_player_extend(ns_video_player *player, const guint8 *bytes,
                                gsize len);

ns_texture *ns_video_player_frame_at(ns_video_player *player, double seconds,
                                     gboolean loop, gboolean *out_ended);


G_END_DECLS

#endif
