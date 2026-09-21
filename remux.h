/*
 * remux.h - give a finished, silent MP4 its sound from a WAV.
 *
 * When the lossless sidecar is asked to be the source of the MP4's audio too,
 * nothing is encoded to AAC while recording: the capture writes PCM to the WAV
 * and the MP4 gets video only. Afterwards the WAV is encoded once and muxed in
 * here.
 *
 * This is not a quality trick. The AAC that comes out is the same encoder at
 * the same 192 kbps ceiling over the same samples, so it sounds like the live
 * one. What it buys is that nothing in the audio path has to keep up with
 * anything during the recording - no encoder running against the capture
 * clock, no timeline to pad - so the whole class of live-timing faults simply
 * cannot occur, and the WAV is the authoritative copy either way.
 *
 * The video is copied through compressed, never re-encoded.
 */
#ifndef NITSHOT_REMUX_H
#define NITSHOT_REMUX_H

#include "nitshot.h"

/*
 * Rewrites 'mp4' with the audio from 'wav' added. On success the original path
 * holds the new file. On failure the original is left exactly as it was - a
 * silent video beside a WAV is a worse result than the alternative, but it is
 * still every frame and every sample the user recorded.
 */
BOOL Remux_AddWavAudio(const wchar_t *mp4, const wchar_t *wav);

#endif /* NITSHOT_REMUX_H */
