#ifndef XBOX_INPUT_SDL_H
#define XBOX_INPUT_SDL_H
#include <SDL.h>

/* Called by the SDL window's main thread, never by the guest poller. */
void xbox_InputSdlEvent(const SDL_Event *event);
void xbox_InputSetMainPump(int on);
int xbox_InputAnyOpen(void);
#endif
