/* Controllers XInput does not see -- DualSense, DualSense Edge, DualShock 4,
 * Switch 2 Pro Controller -- read through HID as XInput states, up to four
 * at once; see hid_pads.c. Windows only: elsewhere SDL reads them. */
#ifndef XBOXRECOMP_HID_PADS_H
#define XBOXRECOMP_HID_PADS_H

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>

/* The `nth` (from 0) of the pads connected, in their places' order: its
 * place (0-3) and its latest state, or -1 if fewer are connected
 * (RECOMP_HID_PADS=0: always -1). */
int  xbox_HidPadState(int nth, XINPUT_STATE *state);
/* Its motors, as XInputSetState's: left the large one, right the small. */
void xbox_HidPadRumble(int place, WORD left, WORD right);
/* The Xbox port (0-3) it answers, shown on its lights; -1 none. */
void xbox_HidPadSetPlayer(int place, int player);
#endif

#endif
