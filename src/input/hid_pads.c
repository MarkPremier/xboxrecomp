/*
 * Controllers read natively through HID: Sony's DualSense, DualSense Edge and
 * DualShock 4 (USB or Bluetooth) and Nintendo's Switch 2 Pro Controller (USB),
 * without Steam Input or DS4Windows.
 *
 * Windows' XInput sees only Xbox-style pads, so these were simply not there.
 * Here the first such pad found is read on a thread of its own and handed to
 * the input layer as an XInput state (xbox_HidPadState), so the button map,
 * dead zones and keyboard merge apply to it unchanged; rumble goes back the
 * same way (xbox_HidPadRumble). RECOMP_HID_PADS=0 turns it off.
 *
 * Sony report layouts (byte offsets from the report ID at 0):
 *   DualSense USB 0x01 and Bluetooth 0x31 (one byte further on): sticks LX
 *   LY RX RY, triggers L2 R2, a counter, then three button bytes.
 *   DualShock 4 USB 0x01 and Bluetooth 0x11 (two bytes further on): sticks,
 *   three button bytes, then the triggers. Either pad's short Bluetooth
 *   report 0x01 has the DualShock 4 layout; reading feature report 0x05
 *   (DualSense) or 0x02 (DualShock 4) asks for the full one.
 *   Button bytes: the hat in the low nibble (0 up, clockwise, 8 none),
 *   square 0x10, cross 0x20, circle 0x40, triangle 0x80; L1 0x01, R1 0x02,
 *   create/share 0x10, options 0x20, L3 0x40, R3 0x80; PS 0x01.
 *
 * Switch 2 Pro Controller (as SDL's SDL_hidapi_switch2.c drives it): over USB
 * it reports nothing until a start sequence goes to the bulk pipes of its
 * vendor interface 1, which Windows binds to WinUSB itself (a running Steam
 * holds that interface; then the pad stays silent). Its HID reports, with
 * the leading report ID 0 hidapi drops: buttons in bytes 5-7 (Y 0x01, X 0x02,
 * B 0x04, A 0x08, R 0x40, ZR 0x80; minus 0x01, plus 0x02, right stick 0x04,
 * left stick 0x08; down 0x01, up 0x02, right 0x04, left 0x08, L 0x40, ZL
 * 0x80), the sticks as 12-bit pairs in bytes 11-16. Buttons are placed by
 * position, as on an Xbox pad (the bottom one is A). Rumble is an HD rumble
 * output report 0x02, sent again every 12 ms while it runs.
 */
#include "hid_pads.h"

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <winusb.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "ole32.lib")

enum { PAD_NONE, PAD_DUALSENSE, PAD_DUALSHOCK4, PAD_SWITCH2 };

static const struct { USHORT vendor, product; int kind; const char *name; } k_pads[] = {
    {0x054C, 0x0CE6, PAD_DUALSENSE, "DualSense"},
    {0x054C, 0x0DF2, PAD_DUALSENSE, "DualSense Edge"},
    {0x054C, 0x05C4, PAD_DUALSHOCK4, "DualShock 4"},
    {0x054C, 0x09CC, PAD_DUALSHOCK4, "DualShock 4"},
    {0x054C, 0x0BA0, PAD_DUALSHOCK4, "DualShock 4 (wireless adapter)"},
    {0x057E, 0x2069, PAD_SWITCH2, "Switch 2 Pro Controller"},
};

static CRITICAL_SECTION s_lock;          /* the state */
static CRITICAL_SECTION s_write_lock;    /* the output handle: a Bluetooth write can take a while */
static XINPUT_STATE s_state;             /* the latest report, as XInput */
static volatile LONG s_connected;
static HANDLE s_write = INVALID_HANDLE_VALUE;
static int s_kind, s_bluetooth;
static USHORT s_output_length;           /* the HID output report's size */
static BYTE s_sequence;
static WORD s_rumble_left, s_rumble_right;
static DWORD s_rumble_sent;
static int s_center[4], s_centered;      /* Switch 2 sticks at rest */

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

static SHORT clamp_axis(int v)
{
    return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static SHORT stick(BYTE value, int flip)
{
    int v = ((int)value - 128) * 257;
    return clamp_axis(flip ? -v - 1 : v);
}

static void store(WORD buttons, BYTE l2, BYTE r2, SHORT lx, SHORT ly, SHORT rx, SHORT ry)
{
    EnterCriticalSection(&s_lock);
    s_state.dwPacketNumber++;
    s_state.Gamepad.wButtons = buttons;
    s_state.Gamepad.bLeftTrigger = l2;
    s_state.Gamepad.bRightTrigger = r2;
    s_state.Gamepad.sThumbLX = lx;
    s_state.Gamepad.sThumbLY = ly;
    s_state.Gamepad.sThumbRX = rx;
    s_state.Gamepad.sThumbRY = ry;
    LeaveCriticalSection(&s_lock);
}

static void parse_sony(const BYTE *r, DWORD length)
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
    store(b, l2, r2, stick(sticks[0], 0), stick(sticks[1], 1), stick(sticks[2], 0), stick(sticks[3], 1));
}

/* A 12-bit Switch 2 stick axis from its rest point; up is positive already. */
static SHORT stick12(int raw, int center)
{
    return clamp_axis((raw - center) * 32767 / 1450);
}

static void parse_switch2(const BYTE *r, DWORD length)
{
    int raw[4], k;
    WORD b = 0;
    if (length && r[0] == 0) { r++; length--; }     /* as hidapi hands it to SDL */
    if (length < 17) return;
    raw[0] = r[11] | ((r[12] & 0x0F) << 8);
    raw[1] = (r[12] >> 4) | (r[13] << 4);
    raw[2] = r[14] | ((r[15] & 0x0F) << 8);
    raw[3] = (r[15] >> 4) | (r[16] << 4);
    if (!s_centered) {      /* the sticks at rest when it connects; the middle if one is held */
        for (k = 0; k < 4; k++) s_center[k] = abs(raw[k] - 2048) < 500 ? raw[k] : 2048;
        s_centered = 1;
    }
    if (r[5] & 0x01) b |= XINPUT_GAMEPAD_X;
    if (r[5] & 0x02) b |= XINPUT_GAMEPAD_Y;
    if (r[5] & 0x04) b |= XINPUT_GAMEPAD_A;
    if (r[5] & 0x08) b |= XINPUT_GAMEPAD_B;
    if (r[5] & 0x40) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (r[6] & 0x01) b |= XINPUT_GAMEPAD_BACK;
    if (r[6] & 0x02) b |= XINPUT_GAMEPAD_START;
    if (r[6] & 0x04) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (r[6] & 0x08) b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (r[7] & 0x01) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (r[7] & 0x02) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (r[7] & 0x04) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (r[7] & 0x08) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (r[7] & 0x40) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    store(b, (r[7] & 0x80) ? 255 : 0, (r[5] & 0x80) ? 255 : 0, stick12(raw[0], s_center[0]),
          stick12(raw[1], s_center[1]), stick12(raw[2], s_center[2]), stick12(raw[3], s_center[3]));
}

/* The first device interface of class `guid` whose path holds `match`. */
static int interface_path(const GUID *guid, const wchar_t *match, wchar_t *path, size_t room)
{
    HDEVINFO set = SetupDiGetClassDevsW(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA iface;
    DWORD index;
    int found = 0;
    if (set == INVALID_HANDLE_VALUE) return 0;
    iface.cbSize = sizeof iface;
    for (index = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, guid, index, &iface); index++) {
        BYTE buffer[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
        wchar_t lower[512];
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, sizeof buffer, NULL, NULL) ||
                wcslen(detail->DevicePath) >= room || wcslen(detail->DevicePath) >= 512)
            continue;
        wcscpy(lower, detail->DevicePath);
        _wcslwr(lower);
        if (wcsstr(lower, match)) {
            wcscpy(path, detail->DevicePath);
            found = 1;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

/* The Switch 2 Pro Controller's interface 1 (WinUSB): its device-interface
 * GUID is in the device's registry key, as Windows recorded it on binding. */
static int switch2_bulk_path(wchar_t *path, size_t room)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA info;
    DWORD index;
    int found = 0;
    if (set == INVALID_HANDLE_VALUE) return 0;
    info.cbSize = sizeof info;
    for (index = 0; !found && SetupDiEnumDeviceInfo(set, index, &info); index++) {
        wchar_t id[512], guids[512];
        DWORD type, size;
        HKEY key;
        GUID guid;
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 512, NULL)) continue;
        _wcsupr(id);
        if (!wcsstr(id, L"VID_057E&PID_2069&MI_01")) continue;
        key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        memset(guids, 0, sizeof guids);
        size = sizeof guids - sizeof *guids;
        if (RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, &type, (BYTE *)guids, &size) != ERROR_SUCCESS) {
            size = sizeof guids - sizeof *guids;
            RegQueryValueExW(key, L"DeviceInterfaceGUID", NULL, &type, (BYTE *)guids, &size);
        }
        RegCloseKey(key);
        if (guids[0] && SUCCEEDED(IIDFromString(guids, &guid)))
            found = interface_path(&guid, L"vid_057e&pid_2069&mi_01", path, room);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static HANDLE s_usb_file = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE s_usb;

static void switch2_close(void)
{
    if (s_usb) WinUsb_Free(s_usb);
    s_usb = NULL;
    if (s_usb_file != INVALID_HANDLE_VALUE) CloseHandle(s_usb_file);
    s_usb_file = INVALID_HANDLE_VALUE;
}

/* SDL's start sequence (each command's length is its byte 5 plus 8), then
 * the first player light; each reply is read and dropped. Kept open while
 * the pad is. */
static void switch2_start(void)
{
    static const BYTE commands[][28] = {
        {0x07, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
        {0x0c, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00},
        {0x11, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
        {0x0a, 0x91, 0x00, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x0c, 0x91, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00},
        {0x01, 0x91, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00},
        {0x01, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
        {0x08, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00},
        {0x03, 0x91, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00},
        {0x03, 0x91, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {0x09, 0x91, 0x00, 0x07, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    wchar_t path[512];
    USB_INTERFACE_DESCRIPTOR descriptor;
    UCHAR in = 0, out = 0, e;
    ULONG timeout;
    size_t c;
    if (!switch2_bulk_path(path, sizeof path / sizeof *path)) {
        fprintf(stderr, "[HIDPAD] Switch 2 Pro Controller: its USB interface 1 was not found\n");
        return;
    }
    s_usb_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (s_usb_file == INVALID_HANDLE_VALUE || !WinUsb_Initialize(s_usb_file, &s_usb)) {
        fprintf(stderr, "[HIDPAD] Switch 2 Pro Controller: its USB interface 1 is in use (error %lu; is Steam "
                        "running?)\n", GetLastError());
        s_usb = NULL;
        switch2_close();
        return;
    }
    if (WinUsb_QueryInterfaceSettings(s_usb, 0, &descriptor))
        for (e = 0; e < descriptor.bNumEndpoints; e++) {
            WINUSB_PIPE_INFORMATION pipe;
            if (WinUsb_QueryPipe(s_usb, 0, e, &pipe) && pipe.PipeType == UsbdPipeTypeBulk) {
                if (pipe.PipeId & 0x80) in = pipe.PipeId;
                else out = pipe.PipeId;
            }
        }
    if (!in || !out) {
        fprintf(stderr, "[HIDPAD] Switch 2 Pro Controller: no bulk pipes on interface 1\n");
        switch2_close();
        return;
    }
    timeout = 1000;
    WinUsb_SetPipePolicy(s_usb, out, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
    timeout = 100;
    WinUsb_SetPipePolicy(s_usb, in, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
    for (c = 0; c < sizeof commands / sizeof *commands; c++) {
        BYTE reply[64];
        ULONG done;
        WinUsb_WritePipe(s_usb, out, (PUCHAR)commands[c], commands[c][5] + 8u, &done, NULL);
        WinUsb_ReadPipe(s_usb, in, reply, sizeof reply, &done, NULL);
    }
    fprintf(stderr, "[HIDPAD] Switch 2 Pro Controller: started over USB\n");
}

/* The first known pad's HID path, and which kind it is; 0 if none. */
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
        if (HidD_GetAttributes(probe, &attributes))
            for (p = 0; p < sizeof k_pads / sizeof *k_pads; p++)
                if (attributes.VendorID == k_pads[p].vendor && attributes.ProductID == k_pads[p].product &&
                        wcslen(detail->DevicePath) < room) {
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

static void send_rumble(void);

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
        s_bluetooth = kind != PAD_SWITCH2 && caps.InputReportByteLength != 64;
        s_output_length = caps.OutputReportByteLength;
        s_centered = 0;
        s_rumble_left = s_rumble_right = 0;
        if (kind == PAD_SWITCH2) {
            switch2_start();
        } else {
            /* Bluetooth sends a short report until a calibration feature
             * report is read (0x05; the DualShock 4's over USB is 0x02);
             * then the full one. */
            memset(report, 0, sizeof report);
            report[0] = kind == PAD_DUALSENSE || s_bluetooth ? 0x05 : 0x02;
            HidD_GetFeature(read, report, caps.FeatureReportByteLength && caps.FeatureReportByteLength <= sizeof report
                                          ? caps.FeatureReportByteLength : (report[0] == 0x05 ? 41 : 37));
        }
        EnterCriticalSection(&s_write_lock);
        s_write = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        LeaveCriticalSection(&s_write_lock);
        fprintf(stderr, "[HIDPAD] %s connected over %s\n", name, s_bluetooth ? "Bluetooth" : "USB");
        InterlockedExchange(&s_connected, 1);
        send_rumble();      /* rumble off, the DualShock 4's light bar on */
        while (ReadFile(read, report, caps.InputReportByteLength ? caps.InputReportByteLength : sizeof report,
                        &got, NULL) && got) {
            if (kind == PAD_SWITCH2) {
                parse_switch2(report, got);
                /* HD rumble fades unless sent again */
                if ((s_rumble_left || s_rumble_right) && GetTickCount() - s_rumble_sent >= 12) send_rumble();
            } else {
                parse_sony(report, got);
            }
        }
        InterlockedExchange(&s_connected, 0);
        EnterCriticalSection(&s_lock);
        memset(&s_state.Gamepad, 0, sizeof s_state.Gamepad);
        LeaveCriticalSection(&s_lock);
        EnterCriticalSection(&s_write_lock);
        if (s_write != INVALID_HANDLE_VALUE) CloseHandle(s_write);
        s_write = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&s_write_lock);
        switch2_close();
        CloseHandle(read);
        fprintf(stderr, "[HIDPAD] %s disconnected\n", name);
    }
}

static int s_enabled;

static BOOL CALLBACK hid_pads_start(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    const char *value = getenv("RECOMP_HID_PADS");
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

static int hid_pads_enabled(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, hid_pads_start, NULL, NULL);
    return s_enabled;
}

int xbox_HidPadState(XINPUT_STATE *state)
{
    if (!hid_pads_enabled() || !InterlockedCompareExchange(&s_connected, 0, 0)) return 0;
    EnterCriticalSection(&s_lock);
    *state = s_state;
    LeaveCriticalSection(&s_lock);
    return 1;
}

/* The Switch 2's HD rumble: a high and a low band at fixed frequencies. */
static void encode_hd_rumble(WORD high_freq, WORD high_amp, WORD low_freq, WORD low_amp, BYTE *out)
{
    out[0] = (BYTE)(high_freq & 0xFF);
    out[1] = (BYTE)(((high_amp >> 4) & 0xFC) | ((high_freq >> 8) & 0x03));
    out[2] = (BYTE)((high_amp >> 12) | (low_freq << 4));
    out[3] = (BYTE)((low_amp & 0xC0) | ((low_freq >> 4) & 0x3F));
    out[4] = (BYTE)(low_amp >> 8);
}

static void send_rumble(void)
{
    BYTE data[1024];
    DWORD size, wrote;
    WORD left = s_rumble_left, right = s_rumble_right;
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
    } else if (s_kind == PAD_SWITCH2) {
        data[0] = 0x02;
        data[1] = (BYTE)(0x50 | (s_sequence++ & 0x0F));
        encode_hd_rumble(0x187, (WORD)(right * 29000u / 65535u), 0x112, (WORD)(left * 29000u / 65535u), data + 2);
        memcpy(data + 0x11, data + 1, 6);
        size = 64;
        s_rumble_sent = GetTickCount();
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

void xbox_HidPadRumble(WORD left, WORD right)
{
    if (!hid_pads_enabled()) return;
    s_rumble_left = left;
    s_rumble_right = right;
    send_rumble();
}

#endif
