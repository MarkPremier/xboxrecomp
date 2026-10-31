/*
 * Sony controllers read natively: DualSense, DualSense Edge and DualShock 4,
 * over USB or Bluetooth, without Steam Input or DS4Windows.
 *
 * Windows' XInput sees only Xbox-style pads, so a DualSense was simply not
 * there. Here the first Sony pad found through HID is read on a thread of its
 * own and handed to the input layer as an XInput state (xbox_SonyPadState),
 * so the button map, dead zones and keyboard merge apply to it unchanged;
 * rumble goes back the same way (xbox_SonyPadRumble). RECOMP_SONY=0 turns it
 * off.
 *
 * Report layouts (byte offsets from the report ID at 0):
 *   DualSense USB 0x01 and Bluetooth 0x31 (one byte further on): sticks LX
 *   LY RX RY, triggers L2 R2, a counter, then three button bytes.
 *   DualShock 4 USB 0x01 and Bluetooth 0x11 (two bytes further on): sticks,
 *   three button bytes, then the triggers. Either pad's short Bluetooth
 *   report 0x01 has the DualShock 4 layout; reading feature report 0x05
 *   (DualSense) or 0x02 (DualShock 4) asks for the full one.
 *   Button bytes: the hat in the low nibble (0 up, clockwise, 8 none),
 *   square 0x10, cross 0x20, circle 0x40, triangle 0x80; L1 0x01, R1 0x02,
 *   create/share 0x10, options 0x20, L3 0x40, R3 0x80; PS 0x01.
 */
#include "dualsense.h"

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

enum { PAD_NONE, PAD_DUALSENSE, PAD_DUALSHOCK4 };

static const struct { USHORT product; int kind; const char *name; } k_pads[] = {
    {0x0CE6, PAD_DUALSENSE, "DualSense"},
    {0x0DF2, PAD_DUALSENSE, "DualSense Edge"},
    {0x05C4, PAD_DUALSHOCK4, "DualShock 4"},
    {0x09CC, PAD_DUALSHOCK4, "DualShock 4"},
    {0x0BA0, PAD_DUALSHOCK4, "DualShock 4 (wireless adapter)"},
};

static CRITICAL_SECTION s_lock;          /* the state */
static CRITICAL_SECTION s_write_lock;    /* the output handle: a Bluetooth write can take a while */
static XINPUT_STATE s_state;             /* the latest report, as XInput */
static volatile LONG s_connected;
static HANDLE s_write = INVALID_HANDLE_VALUE;
static int s_kind, s_bluetooth;
static USHORT s_output_length;           /* the HID output report's size */
static BYTE s_sequence;

/* CRC-32 (zlib's) over 0xA2 and the report: Bluetooth output reports carry it. */
static DWORD report_crc(const BYTE *data, size_t size)
{
    DWORD crc = 0xFFFFFFFFu;
    BYTE header = 0xA2;
    size_t i;
    int bit;
    for (i = 0; i <= size; i++) {
        crc ^= i ? data[i - 1] : header;
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static SHORT stick(BYTE value, int flip)
{
    int v = ((int)value - 128) * 257;
    if (flip) v = -v - 1;
    return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void parse(const BYTE *r, DWORD length)
{
    static const WORD hats[9] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT, 0};
    const BYTE *sticks, *buttons;
    BYTE l2, r2;
    WORD b = 0;
    if (r[0] == 0x31 && s_kind == PAD_DUALSENSE && length >= 12) {          /* DualSense Bluetooth */
        sticks = r + 2; l2 = r[6]; r2 = r[7]; buttons = r + 9;
    } else if (r[0] == 0x01 && s_kind == PAD_DUALSENSE && length >= 11 && !s_bluetooth) {
        sticks = r + 1; l2 = r[5]; r2 = r[6]; buttons = r + 8;              /* DualSense USB */
    } else if (r[0] == 0x11 && length >= 12) {                              /* DualShock 4 Bluetooth */
        sticks = r + 3; buttons = r + 7; l2 = r[10]; r2 = r[11];
    } else if (r[0] == 0x01 && length >= 10) {                              /* DS4 USB; either's short report */
        sticks = r + 1; buttons = r + 5; l2 = r[8]; r2 = r[9];
    } else {
        return;
    }
    b |= hats[(buttons[0] & 15) < 8 ? buttons[0] & 15 : 8];
    if (buttons[0] & 0x10) b |= XINPUT_GAMEPAD_X;
    if (buttons[0] & 0x20) b |= XINPUT_GAMEPAD_A;
    if (buttons[0] & 0x40) b |= XINPUT_GAMEPAD_B;
    if (buttons[0] & 0x80) b |= XINPUT_GAMEPAD_Y;
    if (buttons[1] & 0x01) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (buttons[1] & 0x02) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (buttons[1] & 0x10) b |= XINPUT_GAMEPAD_BACK;
    if (buttons[1] & 0x20) b |= XINPUT_GAMEPAD_START;
    if (buttons[1] & 0x40) b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (buttons[1] & 0x80) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    EnterCriticalSection(&s_lock);
    s_state.dwPacketNumber++;
    s_state.Gamepad.wButtons = b;
    s_state.Gamepad.bLeftTrigger = l2;
    s_state.Gamepad.bRightTrigger = r2;
    s_state.Gamepad.sThumbLX = stick(sticks[0], 0);
    s_state.Gamepad.sThumbLY = stick(sticks[1], 1);
    s_state.Gamepad.sThumbRX = stick(sticks[2], 0);
    s_state.Gamepad.sThumbRY = stick(sticks[3], 1);
    LeaveCriticalSection(&s_lock);
}

/* The first Sony pad's HID path, and which kind it is; 0 if none. */
static int find_pad(wchar_t *path, size_t room, const char **name)
{
    GUID hid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA iface;
    DWORD index;
    int kind = PAD_NONE;
    HidD_GetHidGuid(&hid);
    set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return PAD_NONE;
    iface.cbSize = sizeof iface;
    for (index = 0; !kind && SetupDiEnumDeviceInterfaces(set, NULL, &hid, index, &iface); index++) {
        BYTE buffer[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
        HIDD_ATTRIBUTES attributes;
        HANDLE probe;
        size_t p;
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, sizeof buffer, NULL, NULL)) continue;
        probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE) continue;
        attributes.Size = sizeof attributes;
        if (HidD_GetAttributes(probe, &attributes) && attributes.VendorID == 0x054C)
            for (p = 0; p < sizeof k_pads / sizeof *k_pads; p++)
                if (attributes.ProductID == k_pads[p].product && wcslen(detail->DevicePath) < room) {
                    wcscpy(path, detail->DevicePath);
                    kind = k_pads[p].kind;
                    *name = k_pads[p].name;
                    break;
                }
        CloseHandle(probe);
    }
    SetupDiDestroyDeviceInfoList(set);
    return kind;
}

static DWORD WINAPI reader(LPVOID unused)
{
    (void)unused;
    for (;;) {
        wchar_t path[512];
        const char *name = "";
        HANDLE read;
        PHIDP_PREPARSED_DATA preparsed;
        HIDP_CAPS caps;
        BYTE report[1024];
        DWORD got;
        int kind = find_pad(path, sizeof path / sizeof *path, &name);
        if (!kind) { Sleep(1500); continue; }
        read = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (read == INVALID_HANDLE_VALUE) { Sleep(1500); continue; }
        memset(&caps, 0, sizeof caps);
        if (HidD_GetPreparsedData(read, &preparsed)) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }
        s_kind = kind;
        s_bluetooth = caps.InputReportByteLength != 64;
        s_output_length = caps.OutputReportByteLength;
        /* Bluetooth sends a short report until a calibration feature report
         * is read (0x05; the DualShock 4's over USB is 0x02); then the full
         * one. */
        memset(report, 0, sizeof report);
        report[0] = kind == PAD_DUALSENSE || s_bluetooth ? 0x05 : 0x02;
        HidD_GetFeature(read, report, caps.FeatureReportByteLength && caps.FeatureReportByteLength <= sizeof report
                                      ? caps.FeatureReportByteLength : (report[0] == 0x05 ? 41 : 37));
        EnterCriticalSection(&s_write_lock);
        s_write = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        LeaveCriticalSection(&s_write_lock);
        fprintf(stderr, "[SONY] %s connected over %s\n", name, s_bluetooth ? "Bluetooth" : "USB");
        InterlockedExchange(&s_connected, 1);
        xbox_SonyPadRumble(0, 0);   /* the DualShock 4's light bar on, rumble off */
        while (ReadFile(read, report, caps.InputReportByteLength ? caps.InputReportByteLength : sizeof report,
                        &got, NULL) && got)
            parse(report, got);
        InterlockedExchange(&s_connected, 0);
        EnterCriticalSection(&s_lock);
        memset(&s_state.Gamepad, 0, sizeof s_state.Gamepad);
        LeaveCriticalSection(&s_lock);
        EnterCriticalSection(&s_write_lock);
        if (s_write != INVALID_HANDLE_VALUE) CloseHandle(s_write);
        s_write = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&s_write_lock);
        CloseHandle(read);
        fprintf(stderr, "[SONY] %s disconnected\n", name);
    }
}

static int s_enabled;

static BOOL CALLBACK sony_start(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    const char *value = getenv("RECOMP_SONY");
    HANDLE thread;
    (void)once; (void)parameter; (void)context;
    if (value && *value == '0') return TRUE;
    InitializeCriticalSection(&s_lock);
    InitializeCriticalSection(&s_write_lock);
    thread = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    s_enabled = 1;
    return TRUE;
}

static int sony_enabled(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, sony_start, NULL, NULL);
    return s_enabled;
}

int xbox_SonyPadState(XINPUT_STATE *state)
{
    if (!sony_enabled() || !InterlockedCompareExchange(&s_connected, 0, 0)) return 0;
    EnterCriticalSection(&s_lock);
    *state = s_state;
    LeaveCriticalSection(&s_lock);
    return 1;
}

void xbox_SonyPadRumble(WORD left, WORD right)
{
    BYTE data[1024];
    DWORD size, wrote;
    if (!sony_enabled()) return;
    memset(data, 0, sizeof data);
    if (s_kind == PAD_DUALSENSE) {
        BYTE *effects;
        if (s_bluetooth) {      /* sequence in the high nibble, then the 0x10 tag */
            data[0] = 0x31; data[1] = (BYTE)(s_sequence++ << 4); data[2] = 0x10; effects = data + 3; size = 78;
        } else {
            data[0] = 0x02; effects = data + 1; size = 48;
        }
        effects[0] = 0x03;                      /* rumble, the compatible kind */
        effects[2] = (BYTE)(right >> 8);        /* the small motor */
        effects[3] = (BYTE)(left >> 8);         /* the large one */
    } else if (s_kind == PAD_DUALSHOCK4) {
        BYTE *effects;
        if (s_bluetooth) { data[0] = 0x11; data[1] = 0xC0; data[3] = 0x03; effects = data + 6; size = 78; }
        else             { data[0] = 0x05; data[1] = 0x03; effects = data + 4; size = 32; }
        effects[0] = (BYTE)(right >> 8);
        effects[1] = (BYTE)(left >> 8);
        effects[4] = 0x40;                      /* light bar: blue */
    } else {
        return;
    }
    if (s_bluetooth) {
        DWORD crc = report_crc(data, size - 4);
        memcpy(data + size - 4, &crc, 4);
    }
    /* HID writes take the device's own output report size. */
    if (s_output_length > size && s_output_length <= sizeof data) size = s_output_length;
    EnterCriticalSection(&s_write_lock);
    if (s_write != INVALID_HANDLE_VALUE) WriteFile(s_write, data, size, &wrote, NULL);
    LeaveCriticalSection(&s_write_lock);
}

#endif
