#include "xinput_xbox.h"
#include "input_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int ok, const char *name)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
static void key(SDL_Keycode code, int down)
{
    SDL_Event ev = {0};
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.keysym.sym = code;
    xbox_InputSdlEvent(&ev);
}

int main(int argc, char **argv)
{
    int keyboard = !(argc > 1 && !strcmp(argv[1], "--no-keyboard"));
    setenv("RECOMP_KEYBOARD", keyboard ? "1" : "0", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "SDL init: %s\n", SDL_GetError());
        return 1;
    }
    xbox_InputInit();
    xbox_InputSetMainPump(1);
    XBOX_INPUT_STATE state;
    XBOX_INPUT_CAPABILITIES caps;
    check(xbox_InputGetState(4, &state) == ERROR_DEVICE_NOT_CONNECTED,
          "invalid port is rejected");
    check(xbox_InputGetState(0, NULL) == ERROR_DEVICE_NOT_CONNECTED,
          "null state is rejected");
    key(SDLK_RETURN, 1); key(SDLK_z, 1); key(SDLK_KP_8, 1);
    DWORD result = xbox_InputGetState(0, &state);
    if (keyboard) {
        check(result == ERROR_SUCCESS && (state.Gamepad.wButtons & XBOX_GAMEPAD_START) &&
              state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && state.Gamepad.sThumbLY == 32767,
              "SDL key events reach the Xbox pad, with analog pressure and axes");
        DWORD packet = state.dwPacketNumber;
        key(SDLK_z, 0);
        xbox_InputGetState(0, &state);
        check(state.dwPacketNumber != packet && state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0,
              "key release advances the packet and releases the button");
        key(SDLK_LSHIFT, 1); key(SDLK_RSHIFT, 1); key(SDLK_LSHIFT, 0);
        xbox_InputGetState(0, &state);
        check(state.Gamepad.wButtons & XBOX_GAMEPAD_LEFT_THUMB,
              "releasing one modifier keeps its other side held");
        SDL_Event ev = {0}; ev.type = SDL_WINDOWEVENT; ev.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        xbox_InputSdlEvent(&ev);
        xbox_InputGetState(0, &state);
        check(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbLY == 0,
              "losing window focus releases held controls");
        check(xbox_InputIsConnected(0) && xbox_InputGetCapabilities(0, 0, &caps) == ERROR_SUCCESS,
              "keyboard advertises a controller consistently");
    } else {
        check(result == ERROR_DEVICE_NOT_CONNECTED && !xbox_InputIsConnected(0),
              "disabled keyboard does not create a controller");
    }
    check(xbox_InputGetState(1, &state) == ERROR_DEVICE_NOT_CONNECTED,
          "keyboard only supplies player one");

    int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    check(device >= 0, "virtual host controller attached after input initialization");
    if (device >= 0) {
        SDL_Joystick *joystick = SDL_JoystickOpen(device);
        check(joystick != NULL, "virtual host controller opened");
        if (joystick) {
            SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_B, 1);
            SDL_JoystickUpdate();
            key(SDLK_z, 1);
            result = xbox_InputGetState(0, &state);
            check(result == ERROR_SUCCESS && state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255 &&
                  state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == (keyboard ? 255 : 0),
                  "hotplugged controller works and merges the enabled keyboard");
            check(xbox_InputGetState(1, &state) == ERROR_DEVICE_NOT_CONNECTED,
                  "repeated discovery does not duplicate a host controller");
            SDL_JoystickDetachVirtual(device);
            SDL_JoystickUpdate();
            result = xbox_InputGetState(0, &state);
            check(keyboard ? result == ERROR_SUCCESS && state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 0
                           : result == ERROR_DEVICE_NOT_CONNECTED,
                  "unplug removes stale host button state");
            SDL_JoystickClose(joystick);
        }
    }
    xbox_InputSetMainPump(0);
    check(!xbox_InputAnyOpen(), "detached controller is closed");
    SDL_Quit();
    return failures ? 1 : 0;
}
