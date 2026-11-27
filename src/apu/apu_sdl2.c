/* SDL2 implementation of the fork's packet-based APU output interface. */
#include "apu_xaudio2.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    AUDIO_RATE = 48000,
    AUDIO_CHANNELS = 2,
    DEVICE_BUFFER_FRAMES = 512,
    PACKET_FRAMES = 256,
    MAX_QUEUED_PACKETS = 16,
    FRAME_BYTES = AUDIO_CHANNELS * sizeof(int16_t)
};

static SDL_AudioDeviceID device;

int xa2_set_output(int mode)
{
    (void)mode;
    return XA2_OUTPUT_STEREO;
}

int xa2_output(void) { return XA2_OUTPUT_STEREO; }
int xa2_channels(void) { return AUDIO_CHANNELS; }
int xa2_get_buffer_size(void) { return DEVICE_BUFFER_FRAMES; }
int xa2_is_active(void) { return device != 0; }

int xa2_init(void)
{
    if (device)
        return 1;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    const char *headless = getenv("RECOMP_HEADLESS");
    if (headless && *headless && *headless != '0')
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[SDL2] audio init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_AudioSpec want = {
        .freq = AUDIO_RATE,
        .format = AUDIO_S16SYS,
        .channels = AUDIO_CHANNELS,
        .samples = DEVICE_BUFFER_FRAMES,
    };
    device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!device) {
        fprintf(stderr, "[SDL2] opening audio device failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    fprintf(stderr, "[SDL2] audio initialized: %s, %d Hz stereo\n",
            SDL_GetCurrentAudioDriver(), AUDIO_RATE);
    return 1;
}

void xa2_shutdown(void)
{
    if (!device)
        return;
    SDL_CloseAudioDevice(device);
    device = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!device || !samples || num_samples <= 0 ||
        num_samples > DEVICE_BUFFER_FRAMES)
        return -1;

    Uint32 bytes = (Uint32)num_samples * FRAME_BYTES;
    Uint32 queued = SDL_GetQueuedAudioSize(device);
    if (queued + bytes > MAX_QUEUED_PACKETS * PACKET_FRAMES * FRAME_BYTES)
        return 0;
    if (SDL_QueueAudio(device, samples, bytes) != 0) {
        fprintf(stderr, "[SDL2] queuing audio failed: %s\n", SDL_GetError());
        return -1;
    }
    if (SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PAUSED &&
        queued + bytes >= DEVICE_BUFFER_FRAMES * FRAME_BYTES)
        SDL_PauseAudioDevice(device, 0);
    return 1;
}

int xa2_queued(void)
{
    if (!device)
        return -1;
    const Uint32 packet_bytes = PACKET_FRAMES * FRAME_BYTES;
    return (int)((SDL_GetQueuedAudioSize(device) + packet_bytes - 1) / packet_bytes);
}
