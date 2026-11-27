/**
 * Show the guest framebuffer in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest would otherwise own
 * that thread for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * A title opts in by handing its boot sequence to xbox_HostWindowMain()
 * from main(). With RECOMP_FB_WINDOW set (the same switch as the Win32
 * window) it opens the window, starts the guest on a pthread with at least
 * the main thread's stack and, on macOS, its QoS, and pumps events and
 * presents until the guest returns or the window is closed. Without it, or
 * with RECOMP_HEADLESS=1, no display, or SDL video failing, the guest runs
 * on the calling thread exactly as before.
 *
 * Frames come from the executor's flip (xbox_FramebufferWindowPresent, on
 * the executor's thread). The flip copies the finished surface into a free
 * slot of three and publishes it with an index swap under a mutex held for a
 * few instructions, so the executor never waits for the window and the
 * window never reads guest memory the rasteriser may be writing. The surface
 * format is the GDI window's: 640x480, bpp from pitch / 640, 2 = R5G6B5,
 * 4 = X8R8G8B8. The window stays black until the first flip.
 *
 * Closing the window ends the process: the window owns the main thread, and
 * lifted code cannot be stopped mid-flight. atexit handlers still run.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_WINDOW_SCALE=<n> sizes the window (default 2);
 * RECOMP_FULLSCREEN=1 starts in borderless desktop fullscreen.
 */
#if !defined(_WIN32)

#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "video_player.h"
#include "input/input_sdl.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define FB_W 640u
#define FB_H 480u

#ifdef RECOMP_GPU_NATIVE
static SDL_Window *s_gpu_window;
static uint32_t s_gpu_pixels[FB_W * FB_H];
void *xbox_FramebufferWindowHandle(void) { return s_gpu_window; }
const uint32_t *xbox_FramebufferPixels(uint32_t *width, uint32_t *height)
{
    *width = FB_W; *height = FB_H;
    return s_gpu_pixels;
}
#endif

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t bpp;
};

/* What the window takes from SDL, and gives back on every way out. */
#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS)

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static volatile int    s_wake_pending;
static volatile int    s_in_present;         /* executor inside Present */

/* Counted at the flip, read once a second for the title bar. */
static uint64_t s_flips;
static uint32_t s_frame_draws;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va;
    (void)pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it; it
 * sets this just before each present, on the same thread. As fb_present.c. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy) { s_fb_aa_sy = sy ? sy : 1; }

/* The window is opened by xbox_HostWindowMain, on the main thread. */
void xbox_FramebufferWindowStart(void) {}

/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t bpp)
{
    s_slot[s_back].bpp = bpp;          /* only this thread touches s_back */
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    pthread_mutex_unlock(&s_lock);

    /* Wake the main loop, once per batch of frames it has not looked at. */
    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        /* Not queued (full, or filtered): clear the latch, or no frame
         * would wake the loop again before its timeout. */
        if (SDL_PushEvent(&ev) <= 0)
            __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
    }
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    struct fb_slot *s = &s_slot[s_back];
    const uint8_t *src;
    uint32_t bpp = pitch / FB_W, y, x;
    /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * 640 * 4) is shown as
     * 32-bit, every other pixel, as the Windows presenter does. Dropping it
     * left the window black for any title that renders anti-aliased --
     * Turok's 1280x480 surface, for one. */
    uint32_t out_bpp = bpp == 8 ? 4 : bpp;
    size_t need;

    if (bpp != 2 && bpp != 4 && bpp != 8)
        return;
    need = (size_t)FB_W * FB_H * out_bpp;
    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return;
        s->px = n;
        s->cap = need;
    }
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < FB_H; y++) {
        if (bpp == 8) {
            /* 2x2 has twice the rows too: take every other one (s_fb_aa_sy). */
            const uint32_t *row = (const uint32_t *)
                (src + (size_t)y * pitch * s_fb_aa_sy);
            uint32_t *dst = (uint32_t *)(s->px + (size_t)y * FB_W * 4);
            for (x = 0; x < FB_W; x++)
                dst[x] = row[x * 2];
        } else {
            memcpy(s->px + (size_t)y * FB_W * bpp, src + (size_t)y * pitch,
                   (size_t)FB_W * bpp);
        }
    }
#ifdef RECOMP_GPU_NATIVE
    if (out_bpp == 4) memcpy(s_gpu_pixels, s->px, sizeof s_gpu_pixels);
    else for (size_t i = 0; i < FB_W * FB_H; i++) {
        uint32_t p = ((uint16_t *)s->px)[i];
        s_gpu_pixels[i] = 0xff000000u | ((p & 0xf800u) << 8) | ((p & 0x7e0u) << 5) | ((p & 31u) << 3);
    }
#endif
    publish(out_bpp);
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST))
        present_copy(fb_va, pitch);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* Title bar: "<XBE title> | FPS: n | draws: n", as the GDI window shows it.
 * Written by the loader and the flip, read once a second by the main thread. */
static char s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 (the certificate's title) to UTF-8 for SDL. BMP only: a lone
     * surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        pthread_mutex_lock(&s_lock);
        memcpy(s_title, buf, o + 1);
        pthread_mutex_unlock(&s_lock);
    }
}

/* RECOMP_FPS_LOG=<seconds>: one line per period with the flip rate and the
 * worst frame, as fb_present.c logs it. One clock read a flip, so it can stay
 * on while measuring (RECOMP_PB_REPORT_MS times every draw and phase, which
 * is a system call each on a machine whose kernel fell back to HPET). */
static void fps_log(void)
{
    static double period = -1.0, start, last, worst;
    static long long frames;
    struct timespec ts;
    double now;
    if (period < 0.0) {
        const char *e = getenv("RECOMP_FPS_LOG");
        period = e ? atof(e) : 0.0;
    }
    if (period <= 0.0)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (!start) start = now;
    if (last && (now - last) * 1000.0 > worst) worst = (now - last) * 1000.0;
    last = now;
    frames++;
    if (now - start >= period) {
        fprintf(stderr, "[FPS] %.1f fps (%lld frames in %.2f s), worst %.1f ms\n",
                frames / (now - start), frames, now - start, worst);
        start = now;
        frames = 0;
        worst = 0.0;
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
    fps_log();
}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static int s_guest_rc;
static int s_guest_done;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    __atomic_store_n(&s_guest_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* ── The window, on the process main thread ───────────────────────────── */

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    uint32_t tex_bpp = 0;
    uint64_t shown_seq = 0, t_title = 0, flips_title = 0;
    const char *e;
    int vsync, scale, quit = 0, dirty = 1;

    /* Ctrl-C and SIGTERM keep their default meaning; SDL would turn them
     * into an SDL_QUIT that a headless run never reads. Set before the
     * headless return: audio and input start SDL subsystems there too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#ifdef RECOMP_GPU_NATIVE
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);
#endif
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return guest_main();

    e = getenv("RECOMP_PRESENT_VSYNC");
    vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
    e = getenv("RECOMP_WINDOW_SCALE");
    scale = e ? atoi(e) : 2;
    if (scale < 1 || scale > 8)
        scale = 2;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }
    /* Honor the same fullscreen environment setting as the Windows presenter. */
    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef RECOMP_GPU_NATIVE
    window_flags |= SDL_WINDOW_VULKAN;
#endif
    if (env_on("RECOMP_FULLSCREEN"))
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           (int)FB_W * scale, (int)FB_H * scale,
                           window_flags);
#ifdef RECOMP_GPU_NATIVE
    ren = NULL;
    s_gpu_window = win;
    if (!win) {
#else
    ren = win ? SDL_CreateRenderer(win, -1,
                                   vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
              : NULL;
    if (!ren) {
#endif
        fprintf(stderr, "  [FBWIN] SDL window/renderer failed (%s); running headless\n",
                SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    if (ren) SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H);
    {
        SDL_RendererInfo ri;
        if (ren && SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s); guest runs on a pthread\n",
                    FB_W, FB_H, ri.name, vsync ? "on" : "off");
    }

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] SDL_RegisterEvents failed; running headless\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
    SDL_StopTextInput();
    s_guest_main = guest_main;
    /* This thread pumps SDL events from here on, which also updates the
     * game controllers; the input layer then only reads them. */
    __atomic_store_n(&s_guest_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_running, 1, __ATOMIC_SEQ_CST);
    xbox_InputSetMainPump(1);
    if (!start_guest()) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread; running headless\n");
        __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
        xbox_InputSetMainPump(0);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }

    while (!quit && !__atomic_load_n(&s_guest_done, __ATOMIC_ACQUIRE)) {
        SDL_Event ev;
        uint64_t seq, now;
        const uint8_t *px = NULL;
        uint32_t bpp = 0, pitch = 0;

        if (SDL_WaitEventTimeout(&ev, shown_seq ? 100 : 16)) {
            do {
                xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_frame_event)
                    __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    dirty = 1;
            } while (SDL_PollEvent(&ev));
        }

        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        if (seq != shown_seq) {
            shown_seq = seq;
            px = s_slot[s_front].px;
            bpp = s_slot[s_front].bpp;
            pitch = FB_W * bpp;
        }
        if (px && ren) {
            if (!tex || bpp != tex_bpp) {
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, bpp == 4 ? SDL_PIXELFORMAT_RGB888
                                                      : SDL_PIXELFORMAT_RGB565,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        (int)FB_W, (int)FB_H);
                tex_bpp = bpp;
            }
            if (tex)
                SDL_UpdateTexture(tex, NULL, px, (int)pitch);
            dirty = 1;
        }
        if (dirty && ren) {
            dirty = 0;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (tex)
                SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        now = now_ms();
        if (now - t_title >= 1000) {
            uint64_t f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            char tb[192];
            pthread_mutex_lock(&s_lock);
            snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %u", s_title,
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0,
                     (unsigned)__atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
            pthread_mutex_unlock(&s_lock);
            SDL_SetWindowTitle(win, tb);
            t_title = now;
            flips_title = f;
        }
    }

    /* No present may be mid-publish (it pushes an SDL event) once SDL goes. */
    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
    fprintf(stderr, "  [FBWIN] %s\n", quit ? "window closed" : "guest returned");
    fflush(stdout);
    fflush(stderr);

    if (tex)
        SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
#ifdef RECOMP_GPU_NATIVE
    s_gpu_window = NULL;
#endif
    xbox_InputSetMainPump(0);
    SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    if (quit)
        exit(0);
    for (size_t i = 0; i < sizeof(s_slot) / sizeof(s_slot[0]); i++) {
        free(s_slot[i].px);
        s_slot[i] = (struct fb_slot){0};
    }
    s_back = 0;
    s_ready = 1;
    s_front = 2;
    s_ready_seq = 0;
    s_flips = 0;
    s_frame_draws = 0;
    s_wake_pending = 0;
    return s_guest_rc;
}

#endif /* !_WIN32 */
