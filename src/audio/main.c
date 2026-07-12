/* nordstjernen-audio: isolated MP3 / MPEG-1 audio playback helper driven over stdin/stdout. */
#define _GNU_SOURCE
#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef main
#undef main
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "pl_mpeg.h"

#ifndef MINIMP3_FLOAT_OUTPUT
#define MINIMP3_FLOAT_OUTPUT
#endif
#include "minimp3.h"

#ifdef NS_HAVE_LIBAV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#endif

#ifdef NS_HAVE_VORBISFILE
#include <vorbis/vorbisfile.h>
#endif

#ifdef NS_HAVE_OPUSFILE
#include <opusfile.h>
#endif

#define NS_AUDIO_MAX_PLAYERS 16
#define NS_AUDIO_MAX_SECONDS 1800
#define NS_AUDIO_DEVICE_RATE 44100
#define NS_AUDIO_MAX_BYTES   (256u * 1024u * 1024u)
#define NS_AUDIO_MAX_FLOATS  ((size_t)200u * 1024u * 1024u)

typedef struct {
    char    token[64];
    int     used;
    int     playing;
    int     loop;
    float  *pcm;
    size_t  frames;
    size_t  pcm_cap;
    size_t  cursor;
    float   volume;
    int     reached_end;
    char   *tmp_path;
    long    reload_size;
    Uint32  reload_ticks;
#ifdef NS_HAVE_LIBAV
    AVFormatContext *in_fmt;
    AVCodecContext  *in_dec;
    SwrContext      *in_swr;
    AVPacket        *in_pkt;
    AVFrame         *in_frame;
    SDL_AudioStream *in_stream;
    int              in_sidx;
    int              in_rate;
    int              in_ch;
    int64_t          in_known_size;
    double           in_resume_target;
    int              in_resume_pending;
    size_t           in_discard_frames;
    char             in_path[PATH_MAX];
#endif
} ns_audio_player;

static SDL_AudioDeviceID g_dev;
static int               g_dev_ok;
static SDL_mutex        *g_null_lock;
static SDL_Thread       *g_null_thread;
static SDL_atomic_t      g_null_quit;
static ns_audio_player   g_players[NS_AUDIO_MAX_PLAYERS];

#if defined(__GNUC__)
#define NS_AUDIO_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define NS_AUDIO_PRINTF(a, b)
#endif

static void emit(const char *fmt, ...) NS_AUDIO_PRINTF(1, 2);

static void
emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void
audio_lock(void)
{
    if (g_dev_ok) SDL_LockAudioDevice(g_dev);
    else if (g_null_lock) SDL_LockMutex(g_null_lock);
}

static void
audio_unlock(void)
{
    if (g_dev_ok) SDL_UnlockAudioDevice(g_dev);
    else if (g_null_lock) SDL_UnlockMutex(g_null_lock);
}

static ns_audio_player *
player_find(const char *token)
{
    for (int i = 0; i < NS_AUDIO_MAX_PLAYERS; i++)
        if (g_players[i].used && strcmp(g_players[i].token, token) == 0)
            return &g_players[i];
    return NULL;
}

static ns_audio_player *
player_alloc(const char *token)
{
    ns_audio_player *p = player_find(token);
    if (p) return p;
    for (int i = 0; i < NS_AUDIO_MAX_PLAYERS; i++) {
        if (!g_players[i].used) {
            memset(&g_players[i], 0, sizeof g_players[i]);
            g_players[i].used = 1;
            g_players[i].volume = 1.0f;
            snprintf(g_players[i].token, sizeof g_players[i].token, "%s", token);
            return &g_players[i];
        }
    }
    return NULL;
}

#ifdef NS_HAVE_LIBAV
static void
ain_close(ns_audio_player *p)
{
    if (p->in_stream) SDL_FreeAudioStream(p->in_stream);
    if (p->in_swr) swr_free(&p->in_swr);
    if (p->in_frame) av_frame_free(&p->in_frame);
    if (p->in_pkt) av_packet_free(&p->in_pkt);
    if (p->in_dec) avcodec_free_context(&p->in_dec);
    if (p->in_fmt) avformat_close_input(&p->in_fmt);
    p->in_stream = NULL;
    p->in_sidx = -1;
    p->in_path[0] = '\0';
}
#endif

static void
player_release(ns_audio_player *p)
{
    if (!p || !p->used) return;
#ifdef NS_HAVE_LIBAV
    ain_close(p);
#endif
    char *tmp = p->tmp_path;
    free(p->pcm);
    memset(p, 0, sizeof *p);
    if (tmp) {
        remove(tmp);
        free(tmp);
    }
}

static void
audio_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    float *out = (float *)(void *)stream;
    int frame_bytes = (int)(2 * sizeof(float));
    int nframes = len / frame_bytes;
    SDL_memset(stream, 0, (size_t)len);

    for (int i = 0; i < NS_AUDIO_MAX_PLAYERS; i++) {
        ns_audio_player *p = &g_players[i];
        if (!p->used || !p->playing || !p->pcm) continue;
        for (int f = 0; f < nframes; f++) {
            if (p->cursor >= p->frames) {
                if (p->loop && p->frames > 0) {
                    p->cursor = 0;
                } else {
                    p->playing = 0;
                    p->reached_end = 1;
                    break;
                }
            }
            out[f * 2 + 0] += p->pcm[p->cursor * 2 + 0] * p->volume;
            out[f * 2 + 1] += p->pcm[p->cursor * 2 + 1] * p->volume;
            p->cursor++;
        }
    }

    int total = nframes * 2;
    for (int i = 0; i < total; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

#define NS_AUDIO_NULL_FRAMES 1024

static int
null_audio_thread(void *arg)
{
    (void)arg;
    Uint8 buf[NS_AUDIO_NULL_FRAMES * 2 * sizeof(float)];
    Uint64 start = SDL_GetTicks64();
    Uint64 total_frames = 0;
    while (!SDL_AtomicGet(&g_null_quit)) {
        SDL_LockMutex(g_null_lock);
        audio_cb(NULL, buf, (int)sizeof buf);
        SDL_UnlockMutex(g_null_lock);
        total_frames += NS_AUDIO_NULL_FRAMES;
        Uint64 target = start + total_frames * 1000u / NS_AUDIO_DEVICE_RATE;
        Uint64 now = SDL_GetTicks64();
        if (target > now) {
            SDL_Delay((Uint32)(target - now));
        } else if (now - target > 1000) {
            start = now;
            total_frames = 0;
        }
    }
    return 0;
}

static void
null_audio_start(void)
{
    g_null_lock = SDL_CreateMutex();
    if (!g_null_lock) return;
    SDL_AtomicSet(&g_null_quit, 0);
    g_null_thread = SDL_CreateThread(null_audio_thread, "ns-null-audio", NULL);
    if (!g_null_thread) {
        SDL_DestroyMutex(g_null_lock);
        g_null_lock = NULL;
    }
}

static void
null_audio_stop(void)
{
    if (!g_null_thread) return;
    SDL_AtomicSet(&g_null_quit, 1);
    SDL_WaitThread(g_null_thread, NULL);
    g_null_thread = NULL;
    if (g_null_lock) {
        SDL_DestroyMutex(g_null_lock);
        g_null_lock = NULL;
    }
}

static unsigned char *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || (size_t)n > NS_AUDIO_MAX_BYTES) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *bytes = malloc((size_t)n);
    if (!bytes) { fclose(f); return NULL; }
    if (fread(bytes, 1, (size_t)n, f) != (size_t)n) {
        free(bytes); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return bytes;
}

static int
bytes_are_mpeg1(const unsigned char *b, size_t n)
{
    return n >= 4 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x01 &&
           (b[3] == 0xBA || b[3] == 0xB3);
}

static int
decode_mpeg(const unsigned char *bytes, size_t n,
            float **out_pcm, size_t *out_frames, int *out_rate, int *out_ch)
{
    plm_t *plm = plm_create_with_memory((uint8_t *)bytes, n, 0);
    if (!plm) return 0;
    plm_set_video_enabled(plm, 0);
    plm_set_audio_enabled(plm, 1);
    int rate = plm_get_samplerate(plm);
    if (plm_get_num_audio_streams(plm) < 1 || rate <= 0 || rate > 48000) {
        plm_destroy(plm);
        return 0;
    }

    size_t cap = (size_t)rate * 2u * 8u;
    size_t len = 0;
    float *pcm = malloc(cap * sizeof(float));
    if (!pcm) { plm_destroy(plm); return 0; }
    size_t max_floats = (size_t)rate * 2u * NS_AUDIO_MAX_SECONDS;
    if (max_floats > NS_AUDIO_MAX_FLOATS) max_floats = NS_AUDIO_MAX_FLOATS;
    plm_samples_t *s;
    while ((s = plm_decode_audio(plm)) != NULL) {
        size_t add = (size_t)s->count * 2u;
        if (len + add > cap) {
            while (len + add > cap) {
                if (cap > SIZE_MAX / (2u * sizeof(float))) {
                    free(pcm); plm_destroy(plm); return 0;
                }
                cap *= 2;
            }
            float *grown = realloc(pcm, cap * sizeof(float));
            if (!grown) { free(pcm); plm_destroy(plm); return 0; }
            pcm = grown;
        }
        memcpy(pcm + len, s->interleaved, add * sizeof(float));
        len += add;
        if (len >= max_floats) break;
    }
    plm_destroy(plm);
    if (len == 0) { free(pcm); return 0; }

    *out_pcm = pcm;
    *out_frames = len / 2;
    *out_rate = rate;
    *out_ch = 2;
    return 1;
}

static int
decode_mp3(const unsigned char *bytes, size_t n,
           float **out_pcm, size_t *out_frames, int *out_rate, int *out_ch)
{
    if (n > (size_t)INT_MAX) return 0;

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];

    const uint8_t *p = bytes;
    int rem = (int)n;
    int rate = 0, ch = 0;
    size_t cap = 0, len = 0;
    float *pcm = NULL;
    size_t max_floats = 0;

    while (rem > 0) {
        int samples = mp3dec_decode_frame(&dec, p, rem, frame, &info);
        if (info.frame_bytes <= 0) break;
        if (samples > 0) {
            if (rate == 0) {
                rate = info.hz;
                ch = info.channels;
                if (rate <= 0 || ch < 1) { free(pcm); return 0; }
                max_floats = (size_t)rate * (size_t)ch * NS_AUDIO_MAX_SECONDS;
                if (max_floats > NS_AUDIO_MAX_FLOATS)
                    max_floats = NS_AUDIO_MAX_FLOATS;
            }
            if (info.channels == ch && info.hz == rate) {
                size_t add = (size_t)samples * (size_t)ch;
                if (len + add > cap) {
                    size_t want = cap ? cap : (size_t)rate * (size_t)ch;
                    while (len + add > want) {
                        if (want > SIZE_MAX / (2u * sizeof(float))) {
                            free(pcm); return 0;
                        }
                        want *= 2;
                    }
                    float *grown = realloc(pcm, want * sizeof(float));
                    if (!grown) { free(pcm); return 0; }
                    pcm = grown;
                    cap = want;
                }
                memcpy(pcm + len, frame, add * sizeof(float));
                len += add;
                if (len >= max_floats) break;
            }
        }
        p += info.frame_bytes;
        rem -= info.frame_bytes;
    }

    if (rate == 0 || len == 0) { free(pcm); return 0; }

    *out_pcm = pcm;
    *out_frames = len / (size_t)ch;
    *out_rate = rate;
    *out_ch = ch;
    return 1;
}

#ifdef NS_HAVE_VORBISFILE
typedef struct { const unsigned char *data; size_t size; size_t pos; } ns_ogg_mem;

static size_t
ns_ogg_read(void *ptr, size_t size, size_t nmemb, void *src)
{
    ns_ogg_mem *m = src;
    size_t want = size * nmemb;
    size_t avail = m->size - m->pos;
    if (want > avail) want = avail;
    memcpy(ptr, m->data + m->pos, want);
    m->pos += want;
    return (size != 0) ? want / size : 0;
}

static int
ns_ogg_seek(void *src, ogg_int64_t offset, int whence)
{
    ns_ogg_mem *m = src;
    ogg_int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (ogg_int64_t)m->pos;
    else if (whence == SEEK_END) base = (ogg_int64_t)m->size;
    else return -1;
    ogg_int64_t np = base + offset;
    if (np < 0 || np > (ogg_int64_t)m->size) return -1;
    m->pos = (size_t)np;
    return 0;
}

static long
ns_ogg_tell(void *src)
{
    ns_ogg_mem *m = src;
    return (long)m->pos;
}

static int
bytes_are_ogg(const unsigned char *b, size_t n)
{
    return n >= 4 && b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S';
}

static int
decode_vorbis(const unsigned char *bytes, size_t n,
              float **out_pcm, size_t *out_frames, int *out_rate, int *out_ch)
{
    ns_ogg_mem mem = { bytes, n, 0 };
    ov_callbacks cb = { ns_ogg_read, ns_ogg_seek, NULL, ns_ogg_tell };
    OggVorbis_File vf;
    if (ov_open_callbacks(&mem, &vf, NULL, 0, cb) < 0) return 0;

    vorbis_info *vi = ov_info(&vf, -1);
    if (!vi || vi->rate <= 0 || vi->channels < 1) { ov_clear(&vf); return 0; }
    int rate = (int)vi->rate;
    int ch = vi->channels;

    size_t max_floats = (size_t)rate * (size_t)ch * NS_AUDIO_MAX_SECONDS;
    if (max_floats > NS_AUDIO_MAX_FLOATS) max_floats = NS_AUDIO_MAX_FLOATS;

    float *pcm = NULL;
    size_t cap = 0, len = 0;
    int bitstream = 0;
    for (;;) {
        float **chans = NULL;
        long got = ov_read_float(&vf, &chans, 4096, &bitstream);
        if (got == 0) break;
        if (got < 0) continue;
        size_t add = (size_t)got * (size_t)ch;
        if (len + add > cap) {
            size_t want = cap ? cap : (size_t)rate * (size_t)ch;
            while (len + add > want) {
                if (want > SIZE_MAX / (2u * sizeof(float))) { free(pcm); ov_clear(&vf); return 0; }
                want *= 2;
            }
            float *grown = realloc(pcm, want * sizeof(float));
            if (!grown) { free(pcm); ov_clear(&vf); return 0; }
            pcm = grown;
            cap = want;
        }
        for (long i = 0; i < got; i++)
            for (int c = 0; c < ch; c++)
                pcm[len + (size_t)i * (size_t)ch + (size_t)c] = chans[c][i];
        len += add;
        if (len >= max_floats) break;
    }
    ov_clear(&vf);

    if (len == 0) { free(pcm); return 0; }
    *out_pcm = pcm;
    *out_frames = len / (size_t)ch;
    *out_rate = rate;
    *out_ch = ch;
    return 1;
}
#endif

#ifdef NS_HAVE_OPUSFILE
static int
decode_opus(const unsigned char *bytes, size_t n,
            float **out_pcm, size_t *out_frames, int *out_rate, int *out_ch)
{
    OggOpusFile *of = op_open_memory(bytes, n, NULL);
    if (!of) return 0;
    int ch = op_channel_count(of, -1);
    if (ch < 1 || ch > 8) { op_free(of); return 0; }
    int rate = 48000;

    size_t max_floats = (size_t)rate * (size_t)ch * NS_AUDIO_MAX_SECONDS;
    if (max_floats > NS_AUDIO_MAX_FLOATS) max_floats = NS_AUDIO_MAX_FLOATS;

    int frame_cap = 5760 * ch;
    float *frame = malloc((size_t)frame_cap * sizeof(float));
    if (!frame) { op_free(of); return 0; }

    float *pcm = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        int got = op_read_float(of, frame, frame_cap, NULL);
        if (got == 0) break;
        if (got < 0) { free(frame); free(pcm); op_free(of); return 0; }
        size_t add = (size_t)got * (size_t)ch;
        if (len + add > cap) {
            size_t want = cap ? cap : (size_t)rate * (size_t)ch;
            while (len + add > want) {
                if (want > SIZE_MAX / (2u * sizeof(float))) {
                    free(frame); free(pcm); op_free(of); return 0;
                }
                want *= 2;
            }
            float *grown = realloc(pcm, want * sizeof(float));
            if (!grown) { free(frame); free(pcm); op_free(of); return 0; }
            pcm = grown;
            cap = want;
        }
        memcpy(pcm + len, frame, add * sizeof(float));
        len += add;
        if (len >= max_floats) break;
    }
    free(frame);
    op_free(of);

    if (len == 0) { free(pcm); return 0; }
    *out_pcm = pcm;
    *out_frames = len / (size_t)ch;
    *out_rate = rate;
    *out_ch = ch;
    return 1;
}
#endif

#ifdef NS_HAVE_LIBAV
static int
bytes_are_container(const unsigned char *b, size_t n)
{
    if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3)
        return 1;
    if (n >= 4 && b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S')
        return 1;
    if (n >= 8 && b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p')
        return 1;
    return 0;
}

typedef struct { const unsigned char *data; size_t len; size_t pos; } ah_memsrc;

static int
ah_read(void *opaque, uint8_t *buf, int sz)
{
    ah_memsrc *m = opaque;
    size_t avail = m->len - m->pos;
    if (avail == 0) return AVERROR_EOF;
    if ((size_t)sz > avail) sz = (int)avail;
    memcpy(buf, m->data + m->pos, (size_t)sz);
    m->pos += (size_t)sz;
    return sz;
}

static int64_t
ah_seek(void *opaque, int64_t off, int whence)
{
    ah_memsrc *m = opaque;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return (int64_t)m->len;
    int64_t base = (whence == SEEK_CUR) ? (int64_t)m->pos
                 : (whence == SEEK_END) ? (int64_t)m->len : 0;
    int64_t np = base + off;
    if (np < 0 || (size_t)np > m->len) return -1;
    m->pos = (size_t)np;
    return np;
}

static int
decode_libav(const unsigned char *bytes, size_t n,
             float **out_pcm, size_t *out_frames, int *out_rate, int *out_ch)
{
    av_log_set_level(AV_LOG_QUIET);
    ah_memsrc src = { bytes, n, 0 };
    int ret = 0;
    AVFormatContext *fmt = NULL;
    AVIOContext *avio = NULL;
    AVCodecContext *dec = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    const AVCodec *codec = NULL;
    float *pcm = NULL;
    size_t cap = 0, len = 0;
    int sidx = -1, rate = 0, ch = 0;

    unsigned char *iobuf = av_malloc(32768);
    if (!iobuf) return 0;
    avio = avio_alloc_context(iobuf, 32768, 0, &src, ah_read, NULL, ah_seek);
    if (!avio) { av_free(iobuf); return 0; }

    fmt = avformat_alloc_context();
    if (!fmt) { av_freep(&avio->buffer); avio_context_free(&avio); return 0; }
    fmt->pb = avio;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        av_freep(&avio->buffer); avio_context_free(&avio); return 0;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;

    sidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (sidx < 0 || !codec) goto done;

    dec = avcodec_alloc_context3(codec);
    if (!dec) goto done;
    if (avcodec_parameters_to_context(dec, fmt->streams[sidx]->codecpar) < 0)
        goto done;
    if (avcodec_open2(dec, codec, NULL) < 0) goto done;

    rate = dec->sample_rate;
    ch = dec->ch_layout.nb_channels;
    if (rate <= 0 || rate > 192000 || ch < 1 || ch > 8) goto done;

    swr = swr_alloc();
    if (!swr) goto done;
    av_opt_set_chlayout(swr, "in_chlayout", &dec->ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &dec->ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", rate, 0);
    av_opt_set_int(swr, "out_sample_rate", rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", dec->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(swr) < 0) goto done;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto done;

    *out_rate = rate;
    *out_ch = ch;
    size_t max_floats = (size_t)rate * (size_t)ch * NS_AUDIO_MAX_SECONDS;
    if (max_floats > NS_AUDIO_MAX_FLOATS) max_floats = NS_AUDIO_MAX_FLOATS;
    int eof = 0, capped = 0;
    while (!eof && !capped) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            avcodec_send_packet(dec, NULL);
            eof = 1;
        } else if (pkt->stream_index == sidx) {
            avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
            continue;
        }

        for (;;) {
            int rr = avcodec_receive_frame(dec, frame);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
            if (rr < 0) goto done;

            int out_samples = swr_get_out_samples(swr, frame->nb_samples);
            if (out_samples < 0) { av_frame_unref(frame); goto done; }
            size_t add = (size_t)out_samples * (size_t)ch;
            if (len + add > cap) {
                size_t want = cap ? cap : (size_t)rate * (size_t)ch;
                while (len + add > want) {
                    if (want > SIZE_MAX / (2u * sizeof(float))) {
                        av_frame_unref(frame); goto done;
                    }
                    want *= 2;
                }
                float *grown = realloc(pcm, want * sizeof(float));
                if (!grown) { av_frame_unref(frame); goto done; }
                pcm = grown;
                cap = want;
            }
            uint8_t *dstp = (uint8_t *)(pcm + len);
            int got = swr_convert(swr, &dstp, out_samples,
                                  (const uint8_t **)frame->extended_data,
                                  frame->nb_samples);
            av_frame_unref(frame);
            if (got < 0) goto done;
            len += (size_t)got * (size_t)ch;
            if (len >= max_floats) { capped = 1; break; }
        }
    }

done:
    if (ret == 0 && pcm && len > 0) {
        *out_pcm = pcm;
        *out_frames = len / (size_t)ch;
        ret = 1;
    } else if (!ret) {
        free(pcm);
    }
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (swr) swr_free(&swr);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    return ret;
}

static int64_t
file_size_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static int
file_is_container(const char *path)
{
    unsigned char head[12];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    return bytes_are_container(head, n);
}

static int
pcm_append(ns_audio_player *p, const float *stereo, size_t nframes)
{
    if (!nframes) return 1;
    size_t need = (p->frames + nframes) * 2;
    if (need > NS_AUDIO_MAX_FLOATS) return 0;
    audio_lock();
    if (need > p->pcm_cap) {
        size_t cap = p->pcm_cap ? p->pcm_cap : (size_t)NS_AUDIO_DEVICE_RATE * 2u * 8u;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        float *grown = realloc(p->pcm, cap * sizeof(float));
        if (!grown) {
            audio_unlock();
            return 0;
        }
        p->pcm = grown;
        p->pcm_cap = cap;
    }
    memcpy(p->pcm + p->frames * 2, stereo, nframes * 2 * sizeof(float));
    p->frames += nframes;
    audio_unlock();
    return 1;
}

static void
ain_drain_stream(ns_audio_player *p)
{
    if (!p->in_stream) return;
    float buf[4096];
    for (;;) {
        int avail = SDL_AudioStreamAvailable(p->in_stream);
        if (avail < (int)(2 * sizeof(float))) break;
        int want = avail < (int)sizeof buf ? avail : (int)sizeof buf;
        want -= want % (int)(2 * sizeof(float));
        int got = SDL_AudioStreamGet(p->in_stream, buf, want);
        if (got <= 0) break;
        size_t frames = (size_t)got / (2 * sizeof(float));
        float *ptr = buf;
        if (p->in_discard_frames) {
            size_t drop = p->in_discard_frames < frames
                        ? p->in_discard_frames : frames;
            p->in_discard_frames -= drop;
            ptr += drop * 2;
            frames -= drop;
        }
        if (frames && !pcm_append(p, ptr, frames))
            break;
    }
}

static int
ain_open(ns_audio_player *p, const char *path)
{
    ain_close(p);
    if (avformat_open_input(&p->in_fmt, path, NULL, NULL) < 0) return 0;
    if (avformat_find_stream_info(p->in_fmt, NULL) < 0) { ain_close(p); return 0; }
    const AVCodec *codec = NULL;
    p->in_sidx = av_find_best_stream(p->in_fmt, AVMEDIA_TYPE_AUDIO, -1, -1,
                                     &codec, 0);
    if (p->in_sidx < 0 || !codec) { ain_close(p); return 0; }
    p->in_dec = avcodec_alloc_context3(codec);
    if (!p->in_dec ||
        avcodec_parameters_to_context(
            p->in_dec, p->in_fmt->streams[p->in_sidx]->codecpar) < 0 ||
        avcodec_open2(p->in_dec, codec, NULL) < 0) {
        ain_close(p);
        return 0;
    }
    p->in_rate = p->in_dec->sample_rate;
    p->in_ch = p->in_dec->ch_layout.nb_channels;
    if (p->in_rate <= 0 || p->in_rate > 192000 ||
        p->in_ch < 1 || p->in_ch > 8) {
        ain_close(p);
        return 0;
    }
    p->in_swr = swr_alloc();
    if (!p->in_swr) { ain_close(p); return 0; }
    av_opt_set_chlayout(p->in_swr, "in_chlayout", &p->in_dec->ch_layout, 0);
    av_opt_set_chlayout(p->in_swr, "out_chlayout", &p->in_dec->ch_layout, 0);
    av_opt_set_int(p->in_swr, "in_sample_rate", p->in_rate, 0);
    av_opt_set_int(p->in_swr, "out_sample_rate", p->in_rate, 0);
    av_opt_set_sample_fmt(p->in_swr, "in_sample_fmt", p->in_dec->sample_fmt, 0);
    av_opt_set_sample_fmt(p->in_swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(p->in_swr) < 0) { ain_close(p); return 0; }
    p->in_pkt = av_packet_alloc();
    p->in_frame = av_frame_alloc();
    p->in_stream = SDL_NewAudioStream(AUDIO_F32SYS, (Uint8)p->in_ch,
                                      p->in_rate, AUDIO_F32SYS, 2,
                                      NS_AUDIO_DEVICE_RATE);
    if (!p->in_pkt || !p->in_frame || !p->in_stream) { ain_close(p); return 0; }
    snprintf(p->in_path, sizeof p->in_path, "%s", path);
    p->in_known_size = file_size_of(path);
    return 1;
}

static int
ain_reopen_resume(ns_audio_player *p)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", p->in_path);
    ain_drain_stream(p);
    double decoded_end = (double)p->frames / NS_AUDIO_DEVICE_RATE;
    if (!ain_open(p, path)) return 0;
    if (decoded_end > 0.05) {
        AVRational tb = p->in_fmt->streams[p->in_sidx]->time_base;
        int64_t ts = (int64_t)(decoded_end / av_q2d(tb));
        if (av_seek_frame(p->in_fmt, p->in_sidx, ts,
                          AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(p->in_dec);
            p->in_resume_target = decoded_end;
            p->in_resume_pending = 1;
        }
    }
    return 1;
}

static void
ain_pump(ns_audio_player *p)
{
    if (!p->in_fmt) return;
    int reopened = 0;
    for (;;) {
        int rr = av_read_frame(p->in_fmt, p->in_pkt);
        if (rr < 0) {
            if (!reopened && file_size_of(p->in_path) > p->in_known_size) {
                reopened = 1;
                if (ain_reopen_resume(p)) continue;
            }
            break;
        }
        if (p->in_pkt->stream_index != p->in_sidx) {
            av_packet_unref(p->in_pkt);
            continue;
        }
        avcodec_send_packet(p->in_dec, p->in_pkt);
        av_packet_unref(p->in_pkt);
        while (avcodec_receive_frame(p->in_dec, p->in_frame) == 0) {
            if (p->in_resume_pending) {
                p->in_resume_pending = 0;
                AVRational tb = p->in_fmt->streams[p->in_sidx]->time_base;
                double t0 = p->in_frame->pts != AV_NOPTS_VALUE
                    ? (double)p->in_frame->pts * av_q2d(tb)
                    : p->in_resume_target;
                double ahead = p->in_resume_target - t0;
                p->in_discard_frames = ahead > 0
                    ? (size_t)(ahead * NS_AUDIO_DEVICE_RATE) : 0;
            }
            int out_samples = swr_get_out_samples(p->in_swr,
                                                  p->in_frame->nb_samples);
            if (out_samples > 0) {
                float *conv = malloc((size_t)out_samples * (size_t)p->in_ch *
                                     sizeof(float));
                if (conv) {
                    uint8_t *dstp = (uint8_t *)conv;
                    int got = swr_convert(
                        p->in_swr, &dstp, out_samples,
                        (const uint8_t **)p->in_frame->extended_data,
                        p->in_frame->nb_samples);
                    if (got > 0)
                        SDL_AudioStreamPut(p->in_stream, conv,
                                           got * p->in_ch * (int)sizeof(float));
                    free(conv);
                }
            }
            av_frame_unref(p->in_frame);
            ain_drain_stream(p);
        }
    }
    ain_drain_stream(p);
}
#endif /* NS_HAVE_LIBAV */

static int
resample_to_device(const float *src, size_t src_frames, int src_rate, int src_ch,
                   float **out_pcm, size_t *out_frames)
{
    if (src_ch < 1 || src_rate <= 0 || src_frames == 0) return 0;
    size_t src_bytes = src_frames * (size_t)src_ch * sizeof(float);
    if (src_bytes > (size_t)INT_MAX) return 0;

    SDL_AudioStream *st = SDL_NewAudioStream(
        AUDIO_F32SYS, (Uint8)src_ch, src_rate,
        AUDIO_F32SYS, 2, NS_AUDIO_DEVICE_RATE);
    if (!st) return 0;
    if (SDL_AudioStreamPut(st, src, (int)src_bytes) < 0) {
        SDL_FreeAudioStream(st); return 0;
    }
    SDL_AudioStreamFlush(st);
    int avail = SDL_AudioStreamAvailable(st);
    if (avail <= 0) { SDL_FreeAudioStream(st); return 0; }
    float *buf = malloc((size_t)avail);
    if (!buf) { SDL_FreeAudioStream(st); return 0; }
    int got = SDL_AudioStreamGet(st, buf, avail);
    SDL_FreeAudioStream(st);
    if (got <= 0) { free(buf); return 0; }

    *out_pcm = buf;
    *out_frames = (size_t)got / (2 * sizeof(float));
    return 1;
}

static int
load_audio(ns_audio_player *p, const char *path)
{
    size_t n = 0;
    unsigned char *bytes = read_file(path, &n);
    if (!bytes) return 0;

    float *src = NULL;
    size_t src_frames = 0;
    int src_rate = 0, src_ch = 0;
    int ok = 0;
    if (bytes_are_mpeg1(bytes, n))
        ok = decode_mpeg(bytes, n, &src, &src_frames, &src_rate, &src_ch);
#ifdef NS_HAVE_LIBAV
    else if (bytes_are_container(bytes, n))
        ok = decode_libav(bytes, n, &src, &src_frames, &src_rate, &src_ch);
#endif
    if (!ok)
        ok = decode_mp3(bytes, n, &src, &src_frames, &src_rate, &src_ch);
#ifdef NS_HAVE_LIBAV
    if (!ok)
        ok = decode_libav(bytes, n, &src, &src_frames, &src_rate, &src_ch);
#endif
#ifdef NS_HAVE_VORBISFILE
    if (!ok && bytes_are_ogg(bytes, n))
        ok = decode_vorbis(bytes, n, &src, &src_frames, &src_rate, &src_ch);
#endif
#ifdef NS_HAVE_OPUSFILE
    if (!ok)
        ok = decode_opus(bytes, n, &src, &src_frames, &src_rate, &src_ch);
#endif
    free(bytes);
    if (!ok) return 0;

    float *dev = NULL;
    size_t dev_frames = 0;
    ok = resample_to_device(src, src_frames, src_rate, src_ch, &dev, &dev_frames);
    free(src);
    if (!ok) return 0;

    audio_lock();
    p->pcm = dev;
    p->frames = dev_frames;
    p->pcm_cap = dev_frames * 2;
    p->cursor = 0;
    p->reached_end = 0;
    p->playing = 0;
    audio_unlock();
    return 1;
}

typedef struct {
    FILE   *file;
    size_t  len;
} ns_audio_download;

static size_t
curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    ns_audio_download *d = userdata;
    if (!d || !d->file) return 0;
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;
    size_t bytes = size * nmemb;
    if (d->len > NS_AUDIO_MAX_BYTES) return 0;
    if (bytes > NS_AUDIO_MAX_BYTES - d->len) return 0;
    size_t wrote = fwrite(ptr, 1, bytes, d->file);
    if (wrote != bytes) return wrote;
    d->len += wrote;
    return wrote;
}

static const char *
audio_tmp_dir(void)
{
    const char *d = getenv("TMPDIR");
    return (d && *d) ? d : "/tmp";
}

static char *
write_temp_from_url(const char *url)
{
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "%s/nsaudio-XXXXXX", audio_tmp_dir());
#if defined(_WIN32)
    int fd = mkstemp(tmpl);
#else
    int fd = mkostemp(tmpl, O_CLOEXEC);
#endif
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); remove(tmpl); return NULL; }

    int ok = 0;
    CURL *c = curl_easy_init();
    if (c) {
        ns_audio_download dl = { f, 0 };
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &dl);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
#ifdef CURLOPT_PROTOCOLS_STR
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https,data");
#endif
#ifdef CURLOPT_REDIR_PROTOCOLS_STR
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR,
                         strncmp(url, "https://", 8) == 0 ? "https" :
                         strncmp(url, "data:", 5) == 0 ? "data" :
                         "http,https");
#endif
#ifdef CURLOPT_MAXFILESIZE_LARGE
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE,
                         (curl_off_t)NS_AUDIO_MAX_BYTES);
#endif
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Nordstjernen-Audio");
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
        curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
        const char *ca = getenv("CURL_CA_BUNDLE");
        if (!ca || !*ca) ca = getenv("SSL_CERT_FILE");
        if (ca && *ca)
            curl_easy_setopt(c, CURLOPT_CAINFO, ca);
        ok = curl_easy_perform(c) == CURLE_OK;
        curl_easy_cleanup(c);
    }
    fclose(f);
    if (!ok) { remove(tmpl); return NULL; }
    return strdup(tmpl);
}

static const char *
local_path_for(const char *url, char **tmp_out)
{
    *tmp_out = NULL;
    if (strncmp(url, "file://", 7) == 0) {
        const char *p = url + 7;
        if (strncmp(p, "localhost", 9) == 0) p += 9;
        return p;
    }
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0 ||
        strncmp(url, "data:", 5) == 0) {
        char *t = write_temp_from_url(url);
        if (!t) return NULL;
        *tmp_out = t;
        return t;
    }
    return url;
}

#ifdef NS_HAVE_LIBAV
static void
resume_if_grown(ns_audio_player *p)
{
    audio_lock();
    if (p->reached_end && p->cursor < p->frames) {
        p->reached_end = 0;
        p->playing = 1;
    }
    audio_unlock();
}
#endif

static void
cmd_open(const char *token, const char *url)
{
    ns_audio_player *p = player_alloc(token);
    if (!p) { emit("error %s too-many-players", token); return; }
    audio_lock();
    player_release(p);
    audio_unlock();
    p = player_alloc(token);

    if (!g_dev_ok && !g_null_thread) {
        emit("error %s no-audio-device", token);
        return;
    }

    char *tmp = NULL;
    const char *path = local_path_for(url, &tmp);
    if (!path) { emit("error %s fetch-failed", token); return; }
    p->tmp_path = tmp;

#ifdef NS_HAVE_LIBAV
    if (file_is_container(path) && ain_open(p, path)) {
        ain_pump(p);
        emit("meta %s %.3f", token,
             (double)p->frames / NS_AUDIO_DEVICE_RATE);
        return;
    }
#endif

    if (!load_audio(p, path)) {
        emit("error %s decode-failed", token);
        audio_lock();
        player_release(p);
        audio_unlock();
        return;
    }

    double len = (double)p->frames / NS_AUDIO_DEVICE_RATE;
    emit("meta %s %.3f", token, len);
}

static void
cmd_reload(const char *token, const char *url)
{
    ns_audio_player *p = player_find(token);
    if (!p) { cmd_open(token, url); return; }

    char *tmp = NULL;
    const char *path = local_path_for(url, &tmp);
    if (!path) { emit("error %s fetch-failed", token); free(tmp); return; }

#ifdef NS_HAVE_LIBAV
    if (p->in_fmt && strcmp(path, p->in_path) == 0) {
        if (tmp) { unlink(tmp); free(tmp); }
        size_t before = p->frames;
        ain_pump(p);
        if (p->frames != before) {
            resume_if_grown(p);
            emit("meta %s %.3f", token,
                 (double)p->frames / NS_AUDIO_DEVICE_RATE);
        }
        return;
    }
    if (file_is_container(path)) {
        ns_audio_player fresh;
        memset(&fresh, 0, sizeof fresh);
        if (!ain_open(&fresh, path)) {
            emit("error %s decode-failed", token);
            if (tmp) { unlink(tmp); free(tmp); }
            return;
        }
        ain_pump(&fresh);
        audio_lock();
        float *old_pcm = p->pcm;
        char *old_tmp = p->tmp_path;
        ain_close(p);
        p->in_fmt = fresh.in_fmt;
        p->in_dec = fresh.in_dec;
        p->in_swr = fresh.in_swr;
        p->in_pkt = fresh.in_pkt;
        p->in_frame = fresh.in_frame;
        p->in_stream = fresh.in_stream;
        p->in_sidx = fresh.in_sidx;
        p->in_rate = fresh.in_rate;
        p->in_ch = fresh.in_ch;
        p->in_known_size = fresh.in_known_size;
        memcpy(p->in_path, fresh.in_path, sizeof p->in_path);
        p->pcm = fresh.pcm;
        p->frames = fresh.frames;
        p->pcm_cap = fresh.pcm_cap;
        if (p->cursor > p->frames) p->cursor = p->frames;
        if (p->reached_end && p->cursor < p->frames) {
            p->reached_end = 0;
            p->playing = 1;
        }
        p->tmp_path = tmp;
        audio_unlock();
        free(old_pcm);
        if (old_tmp) { unlink(old_tmp); free(old_tmp); }
        emit("meta %s %.3f", token,
             (double)p->frames / NS_AUDIO_DEVICE_RATE);
        return;
    }
#endif

    struct stat st;
    long size_now = stat(path, &st) == 0 ? (long)st.st_size : -1;
    Uint32 now = SDL_GetTicks();
    int behind = p->reached_end || p->cursor + NS_AUDIO_DEVICE_RATE >= p->frames;
    Uint32 min_gap = behind ? 1500 : 6000;
    if (size_now >= 0 && size_now == p->reload_size &&
        p->reload_ticks && now - p->reload_ticks < 30000) {
        if (tmp) { unlink(tmp); free(tmp); }
        return;
    }
    if (p->reload_ticks && now - p->reload_ticks < min_gap) {
        if (tmp) { unlink(tmp); free(tmp); }
        return;
    }

    ns_audio_player fresh;
    memset(&fresh, 0, sizeof fresh);
    if (!load_audio(&fresh, path)) {
        emit("error %s decode-failed", token);
        if (tmp) { unlink(tmp); free(tmp); }
        return;
    }

    audio_lock();
    float *old_pcm = p->pcm;
    char *old_tmp = p->tmp_path;
    p->pcm = fresh.pcm;
    p->frames = fresh.frames;
    p->pcm_cap = fresh.frames * 2;
    if (p->cursor > p->frames) p->cursor = p->frames;
    if (p->reached_end && p->cursor < p->frames) {
        p->reached_end = 0;
        p->playing = 1;
    }
    p->tmp_path = tmp;
    p->reload_size = size_now;
    p->reload_ticks = now;
    audio_unlock();
    free(old_pcm);
    if (old_tmp) { unlink(old_tmp); free(old_tmp); }

    double len = (double)p->frames / NS_AUDIO_DEVICE_RATE;
    emit("meta %s %.3f", token, len);
}

static void
cmd_play(const char *token)
{
    ns_audio_player *p = player_find(token);
    if (!p || !p->pcm) return;
    audio_lock();
    if (p->cursor >= p->frames) p->cursor = 0;
    p->reached_end = 0;
    p->playing = 1;
    audio_unlock();
    emit("playing %s", token);
}

static void
cmd_pause(const char *token)
{
    ns_audio_player *p = player_find(token);
    if (!p || !p->pcm) return;
    audio_lock();
    p->playing = 0;
    audio_unlock();
    emit("paused %s", token);
}

static void
cmd_seek(const char *token, double seconds)
{
    ns_audio_player *p = player_find(token);
    if (!p || !p->pcm) return;
    if (seconds < 0) seconds = 0;
    size_t frame = (size_t)(seconds * NS_AUDIO_DEVICE_RATE);
    audio_lock();
    if (frame > p->frames) frame = p->frames;
    p->cursor = frame;
    p->reached_end = 0;
    audio_unlock();
}

static void
cmd_volume(const char *token, double vol)
{
    ns_audio_player *p = player_find(token);
    if (!p || !p->pcm) return;
    if (vol < 0) vol = 0;
    if (vol > 1) vol = 1;
    audio_lock();
    p->volume = (float)vol;
    audio_unlock();
}

static void
cmd_loop(const char *token, int on)
{
    ns_audio_player *p = player_find(token);
    if (!p) return;
    audio_lock();
    p->loop = on ? 1 : 0;
    audio_unlock();
}

static void
cmd_stop(const char *token)
{
    ns_audio_player *p = player_find(token);
    if (!p) return;
    audio_lock();
    player_release(p);
    audio_unlock();
}

static void
poll_players(void)
{
    struct { char token[64]; double pos; int ended; int active; }
        snap[NS_AUDIO_MAX_PLAYERS];
    int m = 0;
    audio_lock();
    for (int i = 0; i < NS_AUDIO_MAX_PLAYERS; i++) {
        ns_audio_player *p = &g_players[i];
        if (!p->used || !p->pcm) continue;
        if (p->playing || p->reached_end) {
            memcpy(snap[m].token, p->token, sizeof snap[m].token);
            snap[m].pos = (double)p->cursor / NS_AUDIO_DEVICE_RATE;
            snap[m].ended = p->reached_end;
            snap[m].active = p->playing;
            p->reached_end = 0;
            m++;
        }
    }
    audio_unlock();

    for (int i = 0; i < m; i++) {
        if (snap[i].active) emit("pos %s %.3f", snap[i].token, snap[i].pos);
        if (snap[i].ended) emit("ended %s", snap[i].token);
    }
}

static char *
next_token(char **cursor)
{
    char *s = *cursor;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') { *cursor = s; return NULL; }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = '\0'; s++; }
    *cursor = s;
    return start;
}

#ifdef __linux__
void ns_security_add_writable_dir(const char *dir);
void ns_security_sandbox_init(const char *self_exe);
void ns_security_seccomp_init(void);

static void
sandbox_self(void)
{
    ns_security_add_writable_dir(audio_tmp_dir());
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    self[n > 0 ? n : 0] = '\0';
    ns_security_sandbox_init(n > 0 ? self : NULL);
    ns_security_seccomp_init();
}
#endif

int
main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef NS_HAVE_LIBAV
    av_log_set_level(AV_LOG_QUIET);
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want, have;
        SDL_memset(&want, 0, sizeof want);
        want.freq = NS_AUDIO_DEVICE_RATE;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_cb;
        g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_dev) {
            g_dev_ok = 1;
            SDL_PauseAudioDevice(g_dev, 0);
        }
    }
    if (!g_dev_ok) null_audio_start();
    emit("ready %s", g_dev_ok || g_null_thread ? "1" : "0");

#ifdef __linux__
    sandbox_self();
#endif

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        char *cur = line;
        char *op = next_token(&cur);
        if (!op) { poll_players(); continue; }

        if (strcmp(op, "quit") == 0) break;
        if (strcmp(op, "poll") == 0) { poll_players(); continue; }

        char *token = next_token(&cur);
        if (!token) continue;

        if (strcmp(op, "open") == 0) {
            while (*cur == ' ') cur++;
            cmd_open(token, cur);
        } else if (strcmp(op, "play") == 0) {
            cmd_play(token);
        } else if (strcmp(op, "pause") == 0) {
            cmd_pause(token);
        } else if (strcmp(op, "seek") == 0) {
            char *v = next_token(&cur);
            cmd_seek(token, v ? atof(v) : 0.0);
        } else if (strcmp(op, "volume") == 0) {
            char *v = next_token(&cur);
            cmd_volume(token, v ? atof(v) : 1.0);
        } else if (strcmp(op, "loop") == 0) {
            char *v = next_token(&cur);
            cmd_loop(token, v ? atoi(v) : 0);
        } else if (strcmp(op, "stop") == 0) {
            cmd_stop(token);
        } else if (strcmp(op, "reload") == 0) {
            while (*cur == ' ') cur++;
            cmd_reload(token, cur);
        }
    }

    null_audio_stop();
    if (g_dev_ok) {
        SDL_CloseAudioDevice(g_dev);
        g_dev_ok = 0;
    }
    for (int i = 0; i < NS_AUDIO_MAX_PLAYERS; i++)
        if (g_players[i].used) player_release(&g_players[i]);
    SDL_Quit();
    curl_global_cleanup();
    return 0;
}
