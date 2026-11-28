/* Focused-window key and mouse state, shared with the guest through SDL
 * atomics: the POSIX half of what fb_present.c's window records on Windows.
 *
 * The keyboard-and-mouse scheme (RECOMP_KBM, xinput_device.c) reads the
 * mouse as counts moved since its last poll. With RECOMP_KBM on, a click in
 * the window captures the pointer (SDL relative mouse mode: hidden, never
 * stopped by a screen edge; a touchpad works the same way); Alt+Esc or the
 * window losing the focus releases it, and the click that captures is not
 * passed on as a press. */
#include "input_sdl.h"
#include "xinput_xbox.h"
#include <stdio.h>
#include <stdlib.h>

static SDL_atomic_t keys[256];
static SDL_atomic_t mouse_dx, mouse_dy, mouse_wheel;
static SDL_atomic_t mouse_buttons[5], mouse_clicks[5];
static int captured;            /* main thread only */

static int key_to_vk(SDL_Keycode key)
{
    if (key >= SDLK_a && key <= SDLK_z) return key - SDLK_a + 'A';
    if (key >= SDLK_0 && key <= SDLK_9) return key;
    switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB: return VK_TAB;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_KP_2: return VK_NUMPAD2;
    case SDLK_KP_4: return VK_NUMPAD4;
    case SDLK_KP_6: return VK_NUMPAD6;
    case SDLK_KP_8: return VK_NUMPAD8;
    case SDLK_LSHIFT: return 0xA0;
    case SDLK_RSHIFT: return 0xA1;
    case SDLK_LCTRL: return 0xA2;
    case SDLK_RCTRL: return 0xA3;
    case SDLK_LALT: return 0xA4;
    case SDLK_RALT: return 0xA5;
    default: return 0;
    }
}

int xbox_FramebufferKeyDown(int vk)
{
    if (vk == VK_SHIFT) return SDL_AtomicGet(&keys[0xA0]) || SDL_AtomicGet(&keys[0xA1]);
    if (vk == VK_CONTROL) return SDL_AtomicGet(&keys[0xA2]) || SDL_AtomicGet(&keys[0xA3]);
    if (vk == VK_MENU) return SDL_AtomicGet(&keys[0xA4]) || SDL_AtomicGet(&keys[0xA5]);
    return (unsigned)vk < 256 && SDL_AtomicGet(&keys[vk]);
}

/* Counts moved since the last call. */
int xbox_FramebufferMouseDelta(int *dx, int *dy)
{
    int x = SDL_AtomicSet(&mouse_dx, 0), y = SDL_AtomicSet(&mouse_dy, 0);
    if (dx) *dx = x;
    if (dy) *dy = y;
    return x || y;
}

/* 0 left, 1 right, 2 middle, 3 and 4 the side buttons. */
int xbox_FramebufferMouseButton(int which)
{
    return (unsigned)which < 5 && SDL_AtomicGet(&mouse_buttons[which]);
}

/* Presses since the last call. */
int xbox_FramebufferMouseClicks(int which)
{
    return (unsigned)which < 5 ? SDL_AtomicSet(&mouse_clicks[which], 0) : 0;
}

/* Wheel notches since the last call, up positive. */
int xbox_FramebufferMouseWheel(void)
{
    return SDL_AtomicSet(&mouse_wheel, 0);
}

/* The window captures by itself (see the top of this file). */
void xbox_FramebufferMouseCapture(int on) { (void)on; }

static int kbm_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KBM");
        on = v && *v && *v != '0';
    }
    return on;
}

static void release_input(void)
{
    for (int i = 0; i < 256; i++) SDL_AtomicSet(&keys[i], 0);
    for (int i = 0; i < 5; i++) SDL_AtomicSet(&mouse_buttons[i], 0);
    SDL_AtomicSet(&mouse_dx, 0);
    SDL_AtomicSet(&mouse_dy, 0);
}

static void set_capture(int on)
{
    if (on == captured) return;
    if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0 || !on)
        captured = on;
    if (!on) {
        for (int i = 0; i < 5; i++) SDL_AtomicSet(&mouse_buttons[i], 0);
        SDL_AtomicSet(&mouse_dx, 0);
        SDL_AtomicSet(&mouse_dy, 0);
    }
}

static int mouse_index(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:   return 0;
    case SDL_BUTTON_RIGHT:  return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1:     return 3;
    case SDL_BUTTON_X2:     return 4;
    default:                return -1;
    }
}

void xbox_InputSdlEvent(const SDL_Event *event)
{
    switch (event->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int down = event->type == SDL_KEYDOWN, vk;
        if (event->key.repeat) return;
        /* Alt+Esc hands the pointer back to the desktop. */
        if (down && event->key.keysym.sym == SDLK_ESCAPE &&
                (event->key.keysym.mod & KMOD_ALT) && captured) {
            set_capture(0);
            return;
        }
        vk = key_to_vk(event->key.keysym.sym);
        if (vk) {
            SDL_AtomicSet(&keys[vk], down);
            if (down && getenv("RECOMP_KEY_TRACE"))
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", vk);
        }
        break;
    }
    case SDL_MOUSEMOTION:
        if (captured) {
            SDL_AtomicAdd(&mouse_dx, event->motion.xrel);
            SDL_AtomicAdd(&mouse_dy, event->motion.yrel);
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int i = mouse_index(event->button.button);
        int down = event->type == SDL_MOUSEBUTTONDOWN;
        if (kbm_on() && !captured) {
            if (down && event->button.button == SDL_BUTTON_LEFT)
                set_capture(1);          /* this click only captures */
            return;
        }
        if (i >= 0) {
            SDL_AtomicSet(&mouse_buttons[i], down);
            if (down) SDL_AtomicAdd(&mouse_clicks[i], 1);
        }
        break;
    }
    case SDL_MOUSEWHEEL: {
        int y = event->wheel.y;
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
        if (captured || !kbm_on()) SDL_AtomicAdd(&mouse_wheel, y);
        break;
    }
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            set_capture(0);
            release_input();
        }
        break;
    case SDL_QUIT:
        set_capture(0);
        release_input();
        break;
    }
}
