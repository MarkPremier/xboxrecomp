#ifndef NV2A_FRAME_PACING_H
#define NV2A_FRAME_PACING_H
#include <stdint.h>

typedef struct Nv2aFramePacer {
    uint64_t deadline;
    int hz;
} Nv2aFramePacer;

/* Empty flips repeat the previous surface, so retain its classification until
 * another draw replaces it. Otherwise they reset the cap between video draws. */
static inline int nv2a_video_frame_mode(int previous, int drew, int video)
{
    return drew ? video : previous;
}

static inline int nv2a_frame_limit(int global, int video, int is_video)
{
    if (global < 0 || !is_video || video <= 0) return global;
    return global > 0 && global < video ? global : video;
}

/* Return the earliest presentation time. Changing modes starts a fresh
 * schedule; a late frame never accumulates a backlog of missed deadlines. */
static inline uint64_t nv2a_frame_deadline(Nv2aFramePacer *p, int hz, uint64_t now)
{
    uint64_t period, target;
    if (p->hz != hz) { p->hz = hz; p->deadline = 0; }
    if (hz <= 0) { p->deadline = 0; return now; }
    period = UINT64_C(1000000000) / (uint32_t)hz;
    if (!p->deadline || now > p->deadline + period) {
        p->deadline = now + period;
        return now;
    }
    target = p->deadline;
    p->deadline += period;
    return target > now ? target : now;
}

static inline int nv2a_video_texture(uint32_t format, uint32_t width, uint32_t height)
{
    return (format == 0x24u || format == 0x25u) && width >= 320 && height >= 240;
}

static inline int nv2a_video_coverage(float left, float top, float right, float bottom,
                                     float width, float height)
{
    /* Include letterboxed movies, but not small video surfaces in a world. */
    return width > 0 && height > 0 && right - left >= width * 0.9f &&
           bottom - top >= height * 0.5f;
}
#endif
