/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 *
 * The output (xa2_set_output, before xa2_init): stereo, mono, or 5.1 -- the
 * title's own six channels -- and "auto", 7.1 or 5.1 when the Windows device
 * has the speakers for it. 5.1 plays each channel on its own speaker, or
 * folds it to stereo as the console's Dolby image does for its stereo
 * outputs. 7.1 is the same six channels, each surround played on both the
 * side and the back speaker of its side at -3 dB: the console mixes no more
 * than 5.1, so the two pairs carry the same sound. Mono hears the average of
 * left and right, as the console's mono. A device that goes
 * (unplugged, an HDMI display changing mode) or a new Windows default is
 * reopened from the audio thread, a second between tries.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_MAX_CHANNELS  6
#define XA2_BUF_SAMPLES   1024
#define XA2_NUM_BUFS      16 /* queued packets of 256 frames = ~85ms headroom */
#define XA2_RETRY_MS      1000

/* The console's stereo downmix of 5.1: centre and the back pair at -3 dB, no LFE. */
#define XA2_FOLD          0.7071f

#define XA2_FRONT (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY)
#define XA2_BACK  (SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#define XA2_SIDE  (SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)

/* The speaker of each 5.1 source channel, in xa2_submit_samples' order. */
static const DWORD k_xa2_speakers[6] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
                                        SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT};

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][XA2_MAX_CHANNELS];
static int                     g_xa2_mode = XA2_OUTPUT_STEREO;   /* resolved: never auto */
static int                     g_xa2_channels = 2;
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static uint64_t                g_xa2_empty_queue_events;
static DWORD                   g_xa2_last_report;
static int                     g_xa2_started;
static int                     g_xa2_diagnostic;

/* Device loss and default-device changes, flagged from XAudio2's and
 * Windows' threads, acted on by the audio thread (xa2_submit_samples). */
static volatile LONG           g_xa2_lost;
static volatile LONG           g_xa2_default_changed;
static DWORD                   g_xa2_retry_at;
static int                     g_xa2_reopens;

static const char *mode_name(int mode)
{
    return mode == XA2_OUTPUT_SURROUND ? "5.1" : mode == XA2_OUTPUT_7_1 ? "7.1" : mode == XA2_OUTPUT_MONO ? "mono" :
           mode == XA2_OUTPUT_AUTO ? "auto" : "stereo";
}

/* ── XAudio2's critical error: the device is gone ─────────────── */

static void STDMETHODCALLTYPE engine_pass_start(IXAudio2EngineCallback *self) { (void)self; }
static void STDMETHODCALLTYPE engine_pass_end(IXAudio2EngineCallback *self) { (void)self; }
static void STDMETHODCALLTYPE engine_critical_error(IXAudio2EngineCallback *self, HRESULT error)
{
    (void)self;
    fprintf(stderr, "[XA2] audio device lost (0x%08lX); reopening\n", error);
    InterlockedExchange(&g_xa2_lost, 1);
}
static IXAudio2EngineCallbackVtbl g_engine_vtbl = {engine_pass_start, engine_pass_end, engine_critical_error};
static IXAudio2EngineCallback g_engine_callback = {&g_engine_vtbl};

/* ── Windows' default output device changing ──────────────────── */

static const GUID k_clsid_device_enumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID k_iid_device_enumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID k_iid_notification_client =
    {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
static IMMDeviceEnumerator *g_xa2_enumerator;

static HRESULT STDMETHODCALLTYPE notify_query(IMMNotificationClient *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &k_iid_notification_client)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE notify_add_ref(IMMNotificationClient *self) { (void)self; return 1; }
static ULONG STDMETHODCALLTYPE notify_release(IMMNotificationClient *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE notify_state(IMMNotificationClient *self, LPCWSTR id, DWORD state)
{
    (void)self; (void)id; (void)state;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE notify_device(IMMNotificationClient *self, LPCWSTR id)
{
    (void)self; (void)id;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE notify_default(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)self; (void)id;
    if (flow == eRender && role == eConsole) InterlockedExchange(&g_xa2_default_changed, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE notify_property(IMMNotificationClient *self, LPCWSTR id, const PROPERTYKEY key)
{
    (void)self; (void)id; (void)key;
    return S_OK;
}
static IMMNotificationClientVtbl g_notify_vtbl = {notify_query, notify_add_ref, notify_release, notify_state,
                                                  notify_device, notify_device, notify_default, notify_property};
static IMMNotificationClient g_notify_client = {&g_notify_vtbl};

/* ── The device ───────────────────────────────────────────────── */

static int device_has_surround(DWORD mask)
{
    return (mask & XA2_FRONT) == XA2_FRONT && ((mask & XA2_BACK) == XA2_BACK || (mask & XA2_SIDE) == XA2_SIDE);
}

/* 7.1: the front four, a back pair and a side pair. */
static int device_has_eight(DWORD mask)
{
    return (mask & (XA2_FRONT | XA2_BACK | XA2_SIDE)) == (XA2_FRONT | XA2_BACK | XA2_SIDE);
}

/* The output index of `speaker` in `mask`: outputs follow the mask's bit order. */
static int output_of(DWORD mask, DWORD speaker)
{
    int index = 0;
    DWORD bit;
    if (!(mask & speaker)) return -1;
    for (bit = 1; bit < speaker; bit <<= 1)
        if (mask & bit) index++;
    return index;
}

/* How the source's channels reach the device's. 5.1: each on its own speaker
 * (the back pair on the side pair when there is no back pair), or folded to
 * the first two outputs; mono (or a one-channel device): the average of the
 * left and right that stereo would have. */
static void apply_matrix(void)
{
    float matrix[XA2_MAX_CHANNELS * 8];
    XAUDIO2_VOICE_DETAILS details;
    DWORD mask = 0;
    UINT32 outputs, s;
    int mono;

    IXAudio2MasteringVoice_GetVoiceDetails(g_xa2_master, &details);
    IXAudio2MasteringVoice_GetChannelMask(g_xa2_master, &mask);
    outputs = details.InputChannels;
    if (outputs > 8) outputs = 8;
    mono = g_xa2_mode == XA2_OUTPUT_MONO || outputs == 1;
    memset(matrix, 0, sizeof matrix);

    if (g_xa2_channels == 6 && !mono && g_xa2_mode == XA2_OUTPUT_7_1 && device_has_eight(mask)) {
        for (s = 0; s < 6; s++) {
            DWORD speaker = k_xa2_speakers[s];
            int o = output_of(mask, speaker);
            float level = speaker & XA2_BACK ? XA2_FOLD : 1.0f;
            if (o >= 0 && (UINT32)o < outputs) matrix[o * 6 + s] = level;
            if (speaker & XA2_BACK) {
                o = output_of(mask, speaker == SPEAKER_BACK_LEFT ? SPEAKER_SIDE_LEFT : SPEAKER_SIDE_RIGHT);
                if (o >= 0 && (UINT32)o < outputs) matrix[o * 6 + s] = XA2_FOLD;
            }
        }
        fprintf(stderr, "[XA2] output: 7.1, the surrounds on the side and back pairs (%u channels, mask 0x%lX)\n",
                outputs, mask);
    } else if (g_xa2_channels == 6 && !mono && device_has_surround(mask)) {
        int no_back = (mask & XA2_BACK) != XA2_BACK;
        for (s = 0; s < 6; s++) {
            DWORD speaker = k_xa2_speakers[s];
            int o;
            if (no_back && speaker == SPEAKER_BACK_LEFT) speaker = SPEAKER_SIDE_LEFT;
            if (no_back && speaker == SPEAKER_BACK_RIGHT) speaker = SPEAKER_SIDE_RIGHT;
            o = output_of(mask, speaker);
            if (o >= 0 && (UINT32)o < outputs) matrix[o * 6 + s] = 1.0f;
        }
        fprintf(stderr, "[XA2] output: 5.1 on the device's own speakers (%u channels, mask 0x%lX)\n",
                outputs, mask);
    } else if (g_xa2_channels == 6) {
        static const float left[6] = {1, 0, XA2_FOLD, 0, XA2_FOLD, 0};
        static const float right[6] = {0, 1, XA2_FOLD, 0, 0, XA2_FOLD};
        for (s = 0; s < 6; s++) {
            float l = left[s], r = right[s];
            if (mono) l = r = (left[s] + right[s]) * 0.5f;
            matrix[s] = l;
            if (outputs > 1) matrix[6 + s] = r;
        }
        fprintf(stderr, "[XA2] output: 5.1 folded to %s (%u device channels)\n", mono ? "mono" : "stereo", outputs);
    } else {
        for (s = 0; s < 2; s++) {
            matrix[s] = mono ? 0.5f : (s == 0);
            if (outputs > 1) matrix[2 + s] = mono ? 0.5f : (s == 1);
        }
        fprintf(stderr, "[XA2] output: %s (%u device channels)\n", mono ? "mono" : "stereo", outputs);
    }
    IXAudio2SourceVoice_SetOutputMatrix(g_xa2_source, NULL, (UINT32)g_xa2_channels, outputs, matrix,
                                        XAUDIO2_COMMIT_NOW);
}

static void close_device(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_UnregisterForCallbacks(g_xa2, &g_engine_callback);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }
    g_xa2_started = 0;
}

/* The engine, the default device's mastering voice and the source voice in
 * the run's format. 0 (and nothing open) when any fails. */
static int open_device(int quiet)
{
    WAVEFORMATEXTENSIBLE format = { 0 };
    HRESULT hr;

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        if (!quiet) fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        g_xa2 = NULL;
        return 0;
    }
    IXAudio2_RegisterForCallbacks(g_xa2, &g_engine_callback);

    /* Stereo and mono: two channels, which Windows takes to the device as it
     * does any stereo. 5.1: the device's own channels, mapped by apply_matrix. */
    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        g_xa2_channels == 6 ? XAUDIO2_DEFAULT_CHANNELS : 2, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        if (!quiet) fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        close_device();
        return 0;
    }

    format.Format.wFormatTag      = g_xa2_channels == 6 ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
    format.Format.nChannels       = (WORD)g_xa2_channels;
    format.Format.nSamplesPerSec  = XA2_SAMPLE_RATE;
    format.Format.wBitsPerSample  = 16;
    format.Format.nBlockAlign     = (WORD)(g_xa2_channels * 2);
    format.Format.nAvgBytesPerSec = XA2_SAMPLE_RATE * format.Format.nBlockAlign;
    if (g_xa2_channels == 6) {
        static const GUID pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        format.Format.cbSize = sizeof format - sizeof format.Format;
        format.Samples.wValidBitsPerSample = 16;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                               SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        format.SubFormat = pcm;
    }

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        if (!quiet) fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        g_xa2_source = NULL;
        close_device();
        return 0;
    }
    apply_matrix();
    g_xa2_next_buf = 0;
    g_xa2_started = 0;
    return 1;
}

/* ── Choosing the output ──────────────────────────────────────── */

/* Whether the default device has the speakers for 5.1, asked on a thread of
 * its own so the caller's COM apartment is left as it was. */
static DWORD WINAPI probe_thread(LPVOID result)
{
    IXAudio2 *xa = NULL;
    IXAudio2MasteringVoice *master = NULL;
    DWORD mask = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) && xa) {
        if (SUCCEEDED(IXAudio2_CreateMasteringVoice(xa, &master, XAUDIO2_DEFAULT_CHANNELS, XA2_SAMPLE_RATE,
                                                    0, NULL, NULL, 0))) {
            IXAudio2MasteringVoice_GetChannelMask(master, &mask);
            master->lpVtbl->DestroyVoice(master);
        }
        IXAudio2_Release(xa);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    *(DWORD *)result = mask;
    return 0;
}

int xa2_set_output(int mode)
{
    if (mode == XA2_OUTPUT_AUTO) {
        DWORD mask = 0;
        HANDLE thread = CreateThread(NULL, 0, probe_thread, &mask, 0, NULL);
        if (thread) {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
        mode = device_has_eight(mask) ? XA2_OUTPUT_7_1 :
               device_has_surround(mask) ? XA2_OUTPUT_SURROUND : XA2_OUTPUT_STEREO;
        fprintf(stderr, "[XA2] output auto: the device's speakers (mask 0x%lX) -> %s\n", mask, mode_name(mode));
    }
    if (mode != XA2_OUTPUT_SURROUND && mode != XA2_OUTPUT_7_1 && mode != XA2_OUTPUT_MONO) mode = XA2_OUTPUT_STEREO;
    g_xa2_mode = mode;
    g_xa2_channels = mode == XA2_OUTPUT_SURROUND || mode == XA2_OUTPUT_7_1 ? 6 : 2;
    return mode;
}

int xa2_output(void)
{
    return g_xa2_mode;
}

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    if (!open_device(0)) {
        /* Failed initialization still runs on the COM-initializing thread. */
        if (com_initialized) CoUninitialize();
        return 0;
    }

    /* Told when Windows' default output changes, to follow it. */
    if (SUCCEEDED(CoCreateInstance(&k_clsid_device_enumerator, NULL, CLSCTX_ALL, &k_iid_device_enumerator,
                                   (void **)&g_xa2_enumerator)) && g_xa2_enumerator) {
        if (FAILED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(g_xa2_enumerator, &g_notify_client))) {
            IMMDeviceEnumerator_Release(g_xa2_enumerator);
            g_xa2_enumerator = NULL;
        }
    }

    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_empty_queue_events = 0;
    g_xa2_last_report = 0;
    g_xa2_lost = 0;
    g_xa2_default_changed = 0;
    g_xa2_diagnostic = getenv("RECOMP_APU_DIAG") != NULL;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz %s 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, mode_name(g_xa2_mode), XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (g_xa2_enumerator) {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(g_xa2_enumerator, &g_notify_client);
        IMMDeviceEnumerator_Release(g_xa2_enumerator);
        g_xa2_enumerator = NULL;
    }
    close_device();

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written, %d device reopens)\n", g_xa2_frames_written,
                g_xa2_reopens);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* The device lost, or a new default: closed, then reopened -- at once, and
 * after that a second between tries, the samples meanwhile dropped. 1 with
 * a device to play on. */
static int follow_device(void)
{
    static int com_ready;
    static DWORD test_at;
    /* RECOMP_XA2_TEST_LOSS=<seconds> (test): the device taken as lost every
     * that many seconds, as an unplugged one is. */
    if (!test_at) {
        const char *test = getenv("RECOMP_XA2_TEST_LOSS");
        test_at = test && atoi(test) > 0 ? GetTickCount() + (DWORD)atoi(test) * 1000 : 0xFFFFFFFFu;
    }
    if (test_at != 0xFFFFFFFFu && (LONG)(GetTickCount() - test_at) >= 0) {
        test_at += (DWORD)atoi(getenv("RECOMP_XA2_TEST_LOSS")) * 1000;
        fprintf(stderr, "[XA2] test: device lost\n");
        InterlockedExchange(&g_xa2_lost, 1);
    }
    if (InterlockedExchange(&g_xa2_lost, 0) | InterlockedExchange(&g_xa2_default_changed, 0)) {
        close_device();
        g_xa2_retry_at = GetTickCount();
    }
    if (g_xa2_source) return 1;
    if ((LONG)(GetTickCount() - g_xa2_retry_at) < 0) return 0;
    if (!com_ready) {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        com_ready = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    if (open_device(1)) {
        g_xa2_reopens++;
        fprintf(stderr, "[XA2] audio device reopened (%s)\n", mode_name(g_xa2_mode));
        return 1;
    }
    g_xa2_retry_at = GetTickCount() + XA2_RETRY_MS;
    return 0;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    HRESULT hr;

    if (!g_xa2_initialized || !samples || num_samples <= 0 || num_samples > XA2_BUF_SAMPLES) {
        fprintf(stderr, "[XA2] invalid submission or inactive output; audio output stopped\n");
        return -1;
    }
    if (!follow_device()) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;
    if (!state.BuffersQueued) g_xa2_empty_queue_events++;
    /* Leave the source running once started: a transiently empty queue makes
     * XAudio2 emit silence, whereas stopping and re-prefilling it produced an
     * audible "cut off, then reload" gap on every lull (long streaming samples
     * lost their tail). */
    if (g_xa2_diagnostic && GetTickCount() - g_xa2_last_report >= 1000) {
        XAUDIO2_PERFORMANCE_DATA performance;
        g_xa2_last_report = GetTickCount();
        IXAudio2_GetPerformanceData(g_xa2, &performance);
        fprintf(stderr, "[XA2] queued=%u packets=%d empty_queue_events=%llu engine_glitches=%u\n",
                state.BuffersQueued, g_xa2_frames_written,
                (unsigned long long)g_xa2_empty_queue_events,
                performance.GlitchesSinceEngineStarted);
    }

    idx = g_xa2_next_buf;
    memcpy(g_xa2_bufs[idx], samples, num_samples * g_xa2_channels * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = num_samples * g_xa2_channels * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        /* A device going makes this fail before (or without) the critical
         * error: reopened as for one. */
        fprintf(stderr, "[XA2] SubmitSourceBuffer failed: 0x%08lX; reopening the device\n", hr);
        InterlockedExchange(&g_xa2_lost, 1);
        return 0;
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    if (!g_xa2_started) {
        hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        if (FAILED(hr)) {
            fprintf(stderr, "[XA2] Start after prefill failed: 0x%08lX; reopening the device\n", hr);
            InterlockedExchange(&g_xa2_lost, 1);
            return 0;
        }
        g_xa2_started = 1;
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_channels(void)
{
    return g_xa2_channels;
}

int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source || !g_xa2_started) return -1;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_set_output(int mode)                         { (void)mode; return XA2_OUTPUT_STEREO; }
int  xa2_output(void)                                 { return XA2_OUTPUT_STEREO; }
int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
int  xa2_queued(void)                                 { return -1; }
int  xa2_channels(void)                               { return 2; }

#endif /* _WIN32 */
