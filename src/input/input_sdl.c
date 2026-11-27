/* Focused-window key state, shared with the guest through SDL atomics. */
#include "input_sdl.h"
#include "xinput_xbox.h"
#include <stdio.h>
#include <stdlib.h>

static SDL_atomic_t keys[256];

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

static void release_keys(void)
{
    for (int i = 0; i < 256; i++) SDL_AtomicSet(&keys[i], 0);
}

void xbox_InputSdlEvent(const SDL_Event *event)
{
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        if (event->key.repeat) return;
        int vk = key_to_vk(event->key.keysym.sym);
        if (vk) {
            int down = event->type == SDL_KEYDOWN;
            SDL_AtomicSet(&keys[vk], down);
            if (down && getenv("RECOMP_KEY_TRACE"))
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", vk);
        }
    } else if (event->type == SDL_QUIT ||
               (event->type == SDL_WINDOWEVENT &&
                (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                 event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                 event->window.event == SDL_WINDOWEVENT_MINIMIZED))) {
        release_keys();
    }
}
