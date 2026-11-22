/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* The output: auto (7.1 or 5.1 when the Windows device has the speakers for
 * it, otherwise stereo), stereo, 5.1 (the title's own six channels: front
 * left, front right, centre, LFE, back left, back right), 7.1 (the same six,
 * the surround pair spread over the side and back speakers), or mono. */
#define XA2_OUTPUT_AUTO     0
#define XA2_OUTPUT_STEREO   1
#define XA2_OUTPUT_SURROUND 2
#define XA2_OUTPUT_7_1      3
#define XA2_OUTPUT_MONO     4

/* Chooses the output, before xa2_init (stereo if never called), and returns
 * it resolved: auto becomes stereo, 5.1 or 7.1. The console's audio setting
 * should match it -- Dolby Digital for 5.1 and 7.1, mono for mono -- as the
 * title mixes by it. */
int xa2_set_output(int mode);

/* The resolved output. */
int xa2_output(void);

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved 16-bit frames of xa2_channels() channels: 1 accepted,
 * 0 queue full (or no device for now: one lost or changed is reopened from
 * here), -1 error. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Packets submitted and not yet played, or -1 with no XAudio2 output. */
int xa2_queued(void);

/* The source's channels: 6 for 5.1 and 7.1, otherwise 2. */
int xa2_channels(void);

#endif /* APU_XAUDIO2_H */
