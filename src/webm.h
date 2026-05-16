/* Nordstjernen — minimal WebM/Matroska demuxer.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_WEBM_H
#define ND_WEBM_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_webm_track {
    int     width;
    int     height;
    int     sample_rate;
    int     channels;
    gsize   codec_private_len;
    guint8 *codec_private;
    char   *codec_id;
} nd_webm_track;

typedef struct nd_webm_frame {
    const guint8 *data;
    gsize         len;
    gint64        timecode_us;
    gboolean      keyframe;
} nd_webm_frame;

typedef struct nd_webm nd_webm;

nd_webm *nd_webm_open(const guint8 *body, gsize len);
void     nd_webm_close(nd_webm *w);

const nd_webm_track *nd_webm_video_track(const nd_webm *w);
const nd_webm_track *nd_webm_audio_track(const nd_webm *w);

gboolean nd_webm_next_video_frame(nd_webm *w, nd_webm_frame *out);
gboolean nd_webm_next_audio_frame(nd_webm *w, nd_webm_frame *out);

void     nd_webm_seek_start(nd_webm *w);

G_END_DECLS

#endif
