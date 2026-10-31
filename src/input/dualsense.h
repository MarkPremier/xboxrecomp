/* Sony controllers (DualSense, DualSense Edge, DualShock 4) read through HID
 * as XInput states; see dualsense.c. Windows only: elsewhere SDL reads them. */
#ifndef XBOXRECOMP_DUALSENSE_H
#define XBOXRECOMP_DUALSENSE_H

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>

/* 1 and the pad's latest state, if one is connected (RECOMP_SONY=0: never). */
int  xbox_SonyPadState(XINPUT_STATE *state);
/* Its motors, as XInputSetState's: left the large one, right the small. */
void xbox_SonyPadRumble(WORD left, WORD right);
#endif

#endif
