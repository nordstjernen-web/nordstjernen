/* nordstjernen-video: isolated video frame decoding helper driven over stdin/stdout. */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define NS_VIDEO_MAX_PLAYERS 4
#define NS_VIDEO_RING_SLOTS  3
#define NS_VIDEO_MAX_DIM     4096

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t nslots;
    volatile uint32_t latest;
    volatile uint32_t writing;
    uint32_t frame_bytes;
    double   pts[NS_VIDEO_RING_SLOTS];
} ns_vring_hdr;

#define NS_VRING_MAGIC 0x4e535646u

typedef struct {
    char       token[64];
    char       path[PATH_MAX];
    int        used;
    int        playing;
    unsigned long open_seq;
    int        quit;
    int        want_reopen;
    double     seek_to;
    int        want_seek;
    double     cur;
    int64_t    base_us;
    double     duration;
    char       shm_name[64];
    ns_vring_hdr *ring;
    size_t     ring_bytes;
    int        shm_fd;
    void      *shm_map;
    pthread_t  thread;
    pthread_mutex_t lock;
} ns_video_player;

static ns_video_player g_players[NS_VIDEO_MAX_PLAYERS];
static unsigned long   g_open_seq;
static pthread_mutex_t g_emit_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_next_shm;

#if defined(__GNUC__)
#define NS_VIDEO_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define NS_VIDEO_PRINTF(a, b)
#endif

static void emit(const char *fmt, ...) NS_VIDEO_PRINTF(1, 2);

static void
emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_emit_lock);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&g_emit_lock);
    va_end(ap);
}

static int64_t
now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static const char *
local_path_for(const char *url)
{
    if (strncmp(url, "file://", 7) == 0) {
        const char *p = url + 7;
        if (strncmp(p, "localhost", 9) == 0) p += 9;
        return p;
    }
    return url;
}

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    struct SwsContext *sws;
    AVPacket *pkt;
    AVFrame  *frame;
    int       stream;
    AVRational tb;
    int       w, h;
    int64_t   known_size;
} ns_vdec;

static int64_t
file_size_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static int
vdec_file_grew(ns_vdec *d, const char *path)
{
    return file_size_of(path) > d->known_size;
}

static void
vdec_close(ns_vdec *d)
{
    if (d->sws) sws_freeContext(d->sws);
    if (d->dec) avcodec_free_context(&d->dec);
    if (d->fmt) avformat_close_input(&d->fmt);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->frame) av_frame_free(&d->frame);
    memset(d, 0, sizeof *d);
}

static int
vdec_open(ns_vdec *d, const char *path, double *out_dur)
{
    memset(d, 0, sizeof *d);
    if (avformat_open_input(&d->fmt, path, NULL, NULL) < 0) return 0;
    if (avformat_find_stream_info(d->fmt, NULL) < 0) { vdec_close(d); return 0; }
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->stream < 0) { vdec_close(d); return 0; }
    AVStream *vs = d->fmt->streams[d->stream];
    const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) { vdec_close(d); return 0; }
    d->dec = avcodec_alloc_context3(codec);
    if (!d->dec ||
        avcodec_parameters_to_context(d->dec, vs->codecpar) < 0 ||
        avcodec_open2(d->dec, codec, NULL) < 0) {
        vdec_close(d);
        return 0;
    }
    d->w = vs->codecpar->width;
    d->h = vs->codecpar->height;
    if (d->w <= 0 || d->h <= 0 ||
        d->w > NS_VIDEO_MAX_DIM || d->h > NS_VIDEO_MAX_DIM) {
        vdec_close(d);
        return 0;
    }
    d->tb = vs->time_base;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) { vdec_close(d); return 0; }
    d->known_size = file_size_of(path);
    if (out_dur) {
        *out_dur = 0.0;
        if (d->fmt->duration > 0)
            *out_dur = (double)d->fmt->duration / (double)AV_TIME_BASE;
        else if (vs->duration > 0)
            *out_dur = (double)vs->duration * av_q2d(vs->time_base);
    }
    return 1;
}

static int
ring_create(ns_video_player *p, int w, int h)
{
    uint32_t stride = ((uint32_t)w * 4u + 63u) & ~63u;
    uint32_t frame_bytes = stride * (uint32_t)h;
    size_t total = sizeof(ns_vring_hdr) +
                   (size_t)frame_bytes * NS_VIDEO_RING_SLOTS;
    snprintf(p->shm_name, sizeof p->shm_name, "/nsvid-%d-%u",
             (int)getpid(), ++g_next_shm);
#ifdef _WIN32
    HANDLE hmap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     (DWORD)((uint64_t)total >> 32),
                                     (DWORD)(total & 0xffffffffu), p->shm_name);
    if (!hmap) return 0;
    void *map = MapViewOfFile(hmap, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!map) { CloseHandle(hmap); return 0; }
    p->shm_map = hmap;
#else
    int fd = shm_open(p->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)total) != 0) {
        close(fd);
        shm_unlink(p->shm_name);
        return 0;
    }
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        shm_unlink(p->shm_name);
        return 0;
    }
    p->shm_fd = fd;
#endif
    p->ring = map;
    p->ring_bytes = total;
    memset(p->ring, 0, sizeof(ns_vring_hdr));
    p->ring->magic = NS_VRING_MAGIC;
    p->ring->width = (uint32_t)w;
    p->ring->height = (uint32_t)h;
    p->ring->stride = stride;
    p->ring->nslots = NS_VIDEO_RING_SLOTS;
    p->ring->latest = UINT32_MAX;
    p->ring->frame_bytes = frame_bytes;
    return 1;
}

static void
ring_destroy(ns_video_player *p)
{
#ifdef _WIN32
    if (p->ring) UnmapViewOfFile(p->ring);
    if (p->shm_map) CloseHandle(p->shm_map);
    p->shm_map = NULL;
#else
    if (p->ring) munmap(p->ring, p->ring_bytes);
    if (p->shm_fd >= 0) close(p->shm_fd);
    if (p->shm_name[0]) shm_unlink(p->shm_name);
    p->shm_fd = -1;
#endif
    p->ring = NULL;
    p->shm_name[0] = '\0';
}

static void
ring_publish(ns_video_player *p, ns_vdec *d, const AVFrame *frame, double pts)
{
    ns_vring_hdr *r = p->ring;
    uint32_t slot = (r->latest == UINT32_MAX) ? 0
                    : (r->latest + 1) % r->nslots;
    uint8_t *base = (uint8_t *)r + sizeof(ns_vring_hdr) +
                    (size_t)slot * r->frame_bytes;
    d->sws = sws_getCachedContext(d->sws, frame->width, frame->height,
                                  (enum AVPixelFormat)frame->format,
                                  (int)r->width, (int)r->height,
                                  AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                  NULL, NULL, NULL);
    if (!d->sws) return;
    uint8_t *dst[4] = { base, NULL, NULL, NULL };
    int dst_stride[4] = { (int)r->stride, 0, 0, 0 };
    r->writing = slot + 1;
    sws_scale(d->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, dst, dst_stride);
    r->pts[slot] = pts;
    __sync_synchronize();
    r->latest = slot;
    r->writing = 0;
}

static void *
player_thread(void *ud)
{
    ns_video_player *p = ud;
#ifdef __linux__
    char tname[16];
    snprintf(tname, sizeof tname, "decode-%.8s", p->token);
    pthread_setname_np(pthread_self(), tname);
#endif
    ns_vdec d = {0};
    int opened = 0;
    double last_pos_emit = -10.0;
    int64_t eof_since = 0;

    while (1) {
        pthread_mutex_lock(&p->lock);
        int quit = p->quit;
        int playing = p->playing;
        int want_reopen = p->want_reopen;
        int want_seek = p->want_seek;
        double seek_to = p->seek_to;
        p->want_reopen = 0;
        p->want_seek = 0;
        char path[PATH_MAX];
        memcpy(path, p->path, sizeof path);
        pthread_mutex_unlock(&p->lock);

        if (quit) break;

        if (!opened || want_reopen) {
            double keep = p->cur;
            vdec_close(&d);
            double dur = 0.0;
            opened = vdec_open(&d, local_path_for(path), &dur);
            if (!opened) {
                if (!want_reopen)
                    emit("error %s decode-failed", p->token);
                msleep(200);
                continue;
            }
            if (dur > p->duration) p->duration = dur;
            if (!p->ring) {
                if (!ring_create(p, d.w, d.h)) {
                    emit("error %s shm-failed", p->token);
                    vdec_close(&d);
                    opened = 0;
                    msleep(500);
                    continue;
                }
                emit("shm %s %s %u %u %u %.3f", p->token, p->shm_name,
                     p->ring->width, p->ring->height, p->ring->stride,
                     p->duration);
            }
            if (keep > 0.01) {
                want_seek = 1;
                seek_to = keep;
            }
        }

        if (want_seek) {
            int64_t ts = (int64_t)(seek_to / av_q2d(d.tb));
            av_seek_frame(d.fmt, d.stream, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(d.dec);
            pthread_mutex_lock(&p->lock);
            p->cur = seek_to;
            p->base_us = now_us() - (int64_t)(seek_to * 1e6);
            pthread_mutex_unlock(&p->lock);
        }

        if (!playing) {
            msleep(20);
            continue;
        }

        double clock = (double)(now_us() - p->base_us) / 1e6;
        int got = 0;
        double pts = 0.0;
        while (!got) {
            int rr = av_read_frame(d.fmt, d.pkt);
            if (rr < 0) {
                if (vdec_file_grew(&d, local_path_for(path))) {
                    pthread_mutex_lock(&p->lock);
                    p->want_reopen = 1;
                    pthread_mutex_unlock(&p->lock);
                    eof_since = 0;
                    break;
                }
                int at_end = p->duration > 0.0 &&
                             p->cur >= p->duration - 0.5;
                if (at_end) {
                    avcodec_send_packet(d.dec, NULL);
                    while (avcodec_receive_frame(d.dec, d.frame) == 0) {
                        pts = d.frame->pts != AV_NOPTS_VALUE
                              ? (double)d.frame->pts * av_q2d(d.tb) : p->cur;
                        if (pts >= clock - 0.25) { got = 1; break; }
                    }
                    if (!got) {
                        emit("ended %s", p->token);
                        pthread_mutex_lock(&p->lock);
                        p->playing = 0;
                        pthread_mutex_unlock(&p->lock);
                        eof_since = 0;
                    }
                    break;
                }
                if (!eof_since) eof_since = now_us();
                if (now_us() - eof_since > 10000000) {
                    pthread_mutex_lock(&p->lock);
                    p->want_reopen = 1;
                    pthread_mutex_unlock(&p->lock);
                    eof_since = 0;
                }
                msleep(50);
                break;
            }
            if (d.pkt->stream_index != d.stream) {
                av_packet_unref(d.pkt);
                continue;
            }
            avcodec_send_packet(d.dec, d.pkt);
            av_packet_unref(d.pkt);
            while (avcodec_receive_frame(d.dec, d.frame) == 0) {
                pts = d.frame->pts != AV_NOPTS_VALUE
                      ? (double)d.frame->pts * av_q2d(d.tb) : p->cur;
                if (pts >= clock - 0.25) { got = 1; break; }
            }
        }
        if (!got) continue;
        eof_since = 0;

        double due = pts;
        while (1) {
            clock = (double)(now_us() - p->base_us) / 1e6;
            double wait = due - clock;
            if (wait <= 0.002) break;
            pthread_mutex_lock(&p->lock);
            int stop = p->quit || !p->playing || p->want_seek ||
                       p->want_reopen;
            pthread_mutex_unlock(&p->lock);
            if (stop) break;
            msleep(wait > 0.05 ? 40 : (int)(wait * 1000.0));
        }
        pthread_mutex_lock(&p->lock);
        int skip = p->quit || p->want_seek || p->want_reopen;
        pthread_mutex_unlock(&p->lock);
        if (skip) continue;

        ring_publish(p, &d, d.frame, pts);
        p->cur = pts;
        if (pts - last_pos_emit >= 1.0) {
            last_pos_emit = pts;
            emit("pos %s %.3f", p->token, pts);
        }
    }

    vdec_close(&d);
    return NULL;
}

static ns_video_player *
player_find(const char *token)
{
    for (int i = 0; i < NS_VIDEO_MAX_PLAYERS; i++)
        if (g_players[i].used && strcmp(g_players[i].token, token) == 0)
            return &g_players[i];
    return NULL;
}

static void
player_release(ns_video_player *p, int emit_closed)
{
    pthread_mutex_lock(&p->lock);
    p->quit = 1;
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    if (emit_closed)
        emit("closed %s %s", p->token, p->shm_name);
    ring_destroy(p);
    pthread_mutex_destroy(&p->lock);
    p->used = 0;
}

static void
cmd_open(const char *token, const char *url)
{
    ns_video_player *p = player_find(token);
    if (p) player_release(p, 0);
    p = NULL;
    for (int i = 0; i < NS_VIDEO_MAX_PLAYERS; i++)
        if (!g_players[i].used) { p = &g_players[i]; break; }
    if (!p) {
        ns_video_player *victim = NULL;
        for (int i = 0; i < NS_VIDEO_MAX_PLAYERS; i++) {
            ns_video_player *c = &g_players[i];
            if (!victim || (!c->playing && victim->playing) ||
                (c->playing == victim->playing &&
                 c->open_seq < victim->open_seq))
                victim = c;
        }
        player_release(victim, 1);
        p = victim;
    }
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->open_seq = ++g_open_seq;
    p->shm_fd = -1;
    snprintf(p->token, sizeof p->token, "%s", token);
    snprintf(p->path, sizeof p->path, "%s", url);
    pthread_mutex_init(&p->lock, NULL);
    if (pthread_create(&p->thread, NULL, player_thread, p) != 0) {
        pthread_mutex_destroy(&p->lock);
        p->used = 0;
        emit("error %s thread-failed", token);
    }
}

static void
cmd_reload(const char *token, const char *url)
{
    ns_video_player *p = player_find(token);
    if (!p) { cmd_open(token, url); return; }
    pthread_mutex_lock(&p->lock);
    snprintf(p->path, sizeof p->path, "%s", url);
    p->want_reopen = 1;
    pthread_mutex_unlock(&p->lock);
}

static void
cmd_play(const char *token)
{
    ns_video_player *p = player_find(token);
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (!p->playing) {
        p->base_us = now_us() - (int64_t)(p->cur * 1e6);
        p->playing = 1;
    }
    pthread_mutex_unlock(&p->lock);
    emit("playing %s", token);
}

static void
cmd_pause(const char *token)
{
    ns_video_player *p = player_find(token);
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    p->playing = 0;
    pthread_mutex_unlock(&p->lock);
    emit("paused %s", token);
}

static void
cmd_seek(const char *token, double seconds)
{
    ns_video_player *p = player_find(token);
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    p->seek_to = seconds < 0 ? 0 : seconds;
    p->want_seek = 1;
    pthread_mutex_unlock(&p->lock);
}

static void
cmd_resync(const char *token, double seconds)
{
    if (seconds < 0) return;
    ns_video_player *p = player_find(token);
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (p->playing) {
        double clock = (double)(now_us() - p->base_us) / 1e6;
        double drift = seconds - clock;
        double ad = drift < 0 ? -drift : drift;
        if (ad < 0.75)
            p->base_us = now_us() - (int64_t)(seconds * 1e6);
    }
    pthread_mutex_unlock(&p->lock);
}

static void
cmd_stop(const char *token)
{
    ns_video_player *p = player_find(token);
    if (!p) return;
    player_release(p, 1);
}

#ifdef __linux__
void ns_security_add_writable_dir(const char *dir);
void ns_security_sandbox_init(const char *self_exe);
void ns_security_seccomp_init(void);

static void
sandbox_self(void)
{
    ns_security_add_writable_dir("/dev/shm");
    const char *tmp = getenv("TMPDIR");
    ns_security_add_writable_dir(tmp && *tmp ? tmp : "/tmp");
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    self[n > 0 ? n : 0] = '\0';
    ns_security_sandbox_init(n > 0 ? self : NULL);
    ns_security_seccomp_init();
}
#endif

static char *
next_token(char **cur)
{
    char *s = *cur;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return NULL;
    char *tok = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = '\0';
    *cur = s;
    return tok;
}

int
main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    av_log_set_level(AV_LOG_QUIET);
    emit("ready 1");

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
        if (!op) continue;
        if (strcmp(op, "quit") == 0) break;

        char *token = next_token(&cur);
        if (!token) continue;

        if (strcmp(op, "open") == 0) {
            while (*cur == ' ') cur++;
            cmd_open(token, cur);
        } else if (strcmp(op, "reload") == 0) {
            while (*cur == ' ') cur++;
            cmd_reload(token, cur);
        } else if (strcmp(op, "play") == 0) {
            cmd_play(token);
        } else if (strcmp(op, "pause") == 0) {
            cmd_pause(token);
        } else if (strcmp(op, "seek") == 0) {
            char *v = next_token(&cur);
            cmd_seek(token, v ? atof(v) : 0.0);
        } else if (strcmp(op, "resync") == 0) {
            char *v = next_token(&cur);
            cmd_resync(token, v ? atof(v) : 0.0);
        } else if (strcmp(op, "stop") == 0) {
            cmd_stop(token);
        }
    }

    for (int i = 0; i < NS_VIDEO_MAX_PLAYERS; i++)
        if (g_players[i].used) player_release(&g_players[i], 0);
    return 0;
}
