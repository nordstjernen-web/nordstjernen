/* Nordstjernen — shared audio-clock and video-frame queue layouts. */
#ifndef NS_MEDIA_SHM_H
#define NS_MEDIA_SHM_H

#include <stdint.h>

#define NS_AUDIO_CLOCK_MAGIC 0x4e534143u
#define NS_AUDIO_CLOCK_VERSION 1u
#define NS_AUDIO_CLOCK_SLOTS 16u
#define NS_MEDIA_CLOCK_USED 1u
#define NS_MEDIA_CLOCK_PLAYING 2u

typedef struct {
    volatile uint32_t sequence;
    uint32_t flags;
    int64_t monotonic_us;
    double position;
    char token[64];
} ns_audio_clock_slot;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t nslots;
    uint32_t reserved;
    ns_audio_clock_slot slots[NS_AUDIO_CLOCK_SLOTS];
} ns_audio_clock_hdr;

#define NS_VIDEO_RING_MAGIC 0x4e535647u
#define NS_VIDEO_RING_VERSION 2u
#define NS_VIDEO_RING_SLOTS 8u

typedef struct {
    volatile uint32_t sequence;
    uint32_t generation;
    double pts;
    double duration;
} ns_video_ring_slot;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t nslots;
    uint32_t frame_bytes;
    volatile uint32_t published;
    volatile uint32_t released;
    volatile uint32_t generation;
    volatile uint32_t clock_sequence;
    uint32_t clock_flags;
    int64_t clock_monotonic_us;
    double clock_position;
    ns_video_ring_slot slots[NS_VIDEO_RING_SLOTS];
} ns_video_ring_hdr;

#endif
