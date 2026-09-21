/*
 * audio.h - WASAPI capture for the recorder.
 *
 * One source at a time: either what the speakers are playing (a loopback
 * capture of the default render endpoint) or the microphone. Mixing the two
 * would mean resampling and drift-correcting between two independent clocks,
 * which is a bigger job than it looks and is deliberately left out.
 */
#ifndef NITSHOT_AUDIO_H
#define NITSHOT_AUDIO_H

#include "nitshot.h"

typedef enum {
    AUDIO_NONE = 0,
    AUDIO_SYSTEM,        /* loopback of the default playback device */
    AUDIO_MICROPHONE
} AudioSource;

typedef struct {
    UINT32 sampleRate;
    UINT32 channels;     /* 1 or 2 after downmixing */
} AudioFormat;

/*
 * Called from the capture thread with interleaved 16-bit PCM. 'frames' is the
 * count per channel. Must not block for long - the audio engine is waiting.
 */
typedef void (*AudioSinkFn)(void *ctx, const BYTE *pcm16, UINT32 frames);

/*
 * Starts capturing. 'fmt' receives the format the callback will deliver, which
 * is the endpoint's own rate - AAC needs 44100 or 48000, so anything else is
 * refused rather than silently resampled.
 */
BOOL Audio_Start(AudioSource src, AudioFormat *fmt, AudioSinkFn sink, void *ctx);
void Audio_Stop(void);

#endif /* NITSHOT_AUDIO_H */
