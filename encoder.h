/*
 * encoder.h - which video encoder the recorder can actually use.
 *
 * Enumerating MFTs says what a driver advertises, which is not the same thing
 * as what the MP4 sink will accept: this machine's NVIDIA AV1 Encoder MFT
 * enumerates, configures and starts, then fails at Finalize with
 * MF_E_SINK_HEADERS_NOT_FOUND because the in-box MP4 sink cannot mux AV1. So
 * the list offered in Settings is built by *probing* - writing and finalising a
 * one-frame file through the same configuration code recording uses - and not
 * by enumeration.
 *
 * Probing costs around 300 ms per candidate, far too much for startup, so the
 * result is cached in the INI against the adapter and its driver version and
 * only redone when either changes.
 */
#ifndef NITSHOT_ENCODER_H
#define NITSHOT_ENCODER_H

#include "nitshot.h"

#include <mfidl.h>
#include <mfreadwrite.h>

typedef enum {
    ENC_AUTO = 0,   /* pick the best that probed clean; never a probe slot */
    ENC_H264,       /* 8-bit, RGB32 in                                     */
    ENC_HEVC8,      /* 8-bit, RGB32 in                                     */
    ENC_HEVC10,     /* Main10, P010 in, BT.2020 PQ - the HDR one           */
    ENC_COUNT
} EncoderId;

typedef struct {
    BOOL    probed;                 /* the probe has run at least once */
    BOOL    usable[ENC_COUNT];
    BOOL    hardware[ENC_COUNT];
    wchar_t mft[ENC_COUNT][96];     /* friendly name of the encoder that ran */
} EncoderCaps;

/* The cached result. Never NULL; everything is FALSE before the first probe. */
const EncoderCaps *Encoder_Caps(void);

/* Runs the probe unless a matching cache entry exists. Blocking, seconds. */
void Encoder_Probe(BOOL force);

/* Same, on a worker thread, at most one at a time. Used to warm the cache
   without holding anything up. */
void Encoder_ProbeAsync(void);

/*
 * Turns a setting into the encoder that will actually be used: applies the
 * ranking for ENC_AUTO, and walks down to the next usable one when the chosen
 * encoder did not probe clean. Returns ENC_COUNT when nothing works at all.
 */
EncoderId Encoder_Resolve(EncoderId want, BOOL hdr);

/* The next usable encoder below 'id', for recovering at record time. */
EncoderId Encoder_Fallback(EncoderId id);

BOOL           Encoder_IsHdr(EncoderId id);     /* needs the P010 path */
const wchar_t *Encoder_Name(EncoderId id);      /* for the UI and the log */
const wchar_t *Encoder_Token(EncoderId id);     /* for the INI */
EncoderId      Encoder_FromToken(const wchar_t *token);

/*
 * Adds the video stream and sets the input type. These are what the probe
 * exercises, so a combination that probes clean is one that records.
 * 'streamIndex' receives the sink writer's stream index.
 */
BOOL Encoder_AddVideoStream(IMFSinkWriter *writer, EncoderId id,
                            UINT32 width, UINT32 height, int fps,
                            DWORD *streamIndex);

#endif /* NITSHOT_ENCODER_H */
