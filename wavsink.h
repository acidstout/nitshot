/*
 * wavsink.h - a plain 16-bit PCM WAV written alongside a recording.
 *
 * The MP4's audio is AAC, and Media Foundation's AAC encoder stops at 192 kbps
 * (measured: 96, 128, 160 and 192 are the only rates it offers for 48 kHz
 * stereo). That is one lossy generation on top of whatever was playing, and
 * there is no way around it inside the MP4 - a loopback capture hands over
 * decoded PCM, so there is no source bitstream to copy through.
 *
 * So when it matters, the same samples are also written untouched to a .wav
 * beside the .mp4. It is fed from the same place as the encoder, padding
 * included, which keeps it sample-aligned with the video.
 */
#ifndef NITSHOT_WAVSINK_H
#define NITSHOT_WAVSINK_H

#include "nitshot.h"

typedef struct WavSink WavSink;

WavSink *Wav_Create(const wchar_t *path, UINT32 rate, UINT32 channels);

/* 'pcm16' NULL writes 'frames' of silence, matching the recorder's padding. */
BOOL Wav_Write(WavSink *w, const BYTE *pcm16, UINT32 frames);

/* Patches the RIFF sizes and closes. Returns FALSE if nothing usable was
   written, in which case the file is deleted rather than left as a stub. */
BOOL Wav_Close(WavSink *w);

#endif /* NITSHOT_WAVSINK_H */
