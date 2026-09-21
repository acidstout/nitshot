/*
 * record.h - screen recording to MP4.
 *
 * Video comes from Desktop Duplication and audio from WASAPI; both are handed
 * to a Media Foundation sink writer that muxes an MP4.
 *
 * On a display in advanced colour the recorder captures scRGB half-float,
 * converts it to 10-bit BT.2020 PQ and encodes HEVC Main10; otherwise it takes
 * the 8-bit desktop and encodes H.264. Which encoder is available is settled by
 * probing rather than enumeration - see encoder.h.
 */
#ifndef NITSHOT_RECORD_H
#define NITSHOT_RECORD_H

#include "nitshot.h"
#include "audio.h"

typedef struct {
    RECT        region;     /* screen coordinates */
    AudioSource audio;
    int         fps;
} RecordOptions;

typedef struct {
    BOOL    ok;
    wchar_t path[MAX_PATH];
    wchar_t wavPath[MAX_PATH]; /* the lossless sidecar, empty if not written */
    DWORD   seconds;
    wchar_t error[128];     /* empty unless something went wrong */
} RecordResult;

/*
 * Starts recording on a worker thread. When it finishes - because Record_Stop
 * was called or because something failed - 'doneMsg' is posted to 'notify'
 * with a heap-allocated RecordResult as lParam, freed with Record_FreeResult().
 */
BOOL  Record_Start(HWND notify, UINT doneMsg, const RecordOptions *opt);

/* Asks the worker to finish. Returns once it has, so the file is complete. */
void  Record_Stop(void);

BOOL  Record_IsActive(void);
DWORD Record_ElapsedMs(void);

void  Record_FreeResult(RecordResult *res);

#endif /* NITSHOT_RECORD_H */
