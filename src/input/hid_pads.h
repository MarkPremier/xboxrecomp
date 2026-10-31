/* Controllers XInput does not see -- DualSense, DualSense Edge, DualShock 4,
 * Switch 2 Pro Controller -- read through HID as XInput states; see
 * hid_pads.c. Windows only: elsewhere SDL reads them. */
#ifndef XBOXRECOMP_HID_PADS_H
#define XBOXRECOMP_HID_PADS_H

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>

/* 1 and the pad's latest state, if one is connected (RECOMP_HID_PADS=0: never). */
int  xbox_HidPadState(XINPUT_STATE *state);
/* Its motors, as XInputSetState's: left the large one, right the small. */
void xbox_HidPadRumble(WORD left, WORD right);
#endif

#endif
