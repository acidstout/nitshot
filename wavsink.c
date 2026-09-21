/*
 * wavsink.c - the lossless sidecar.
 */

#include "nitshot.h"
#include "wavsink.h"
#include "log.h"

#include <strsafe.h>

/*
 * RIFF sizes are unsigned 32-bit, so a WAV cannot exceed 4 GB however it is
 * written. At 48 kHz stereo that is a little over six hours; the limit is
 * enforced rather than discovered, because overflowing it silently produces a
 * file whose header disagrees with its contents.
 */
#define WAV_MAX_DATA  0xFFFFFF00u

#define HEADER_BYTES  44

struct WavSink {
    HANDLE   file;
    UINT32   rate, channels;
    UINT32   dataBytes;
    BOOL     full;          /* the 4 GB ceiling was reached */
    wchar_t  path[MAX_PATH];
};

static void PutU32(BYTE *p, UINT32 v)
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8);
    p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24);
}

static void PutU16(BYTE *p, UINT16 v)
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8);
}

static void BuildHeader(BYTE *h, UINT32 rate, UINT32 channels, UINT32 dataBytes)
{
    UINT32 block = channels * 2;

    memcpy(h, "RIFF", 4);
    PutU32(h + 4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    PutU32(h + 16, 16);                      /* PCM fmt chunk size */
    PutU16(h + 20, 1);                       /* WAVE_FORMAT_PCM */
    PutU16(h + 22, (UINT16)channels);
    PutU32(h + 24, rate);
    PutU32(h + 28, rate * block);            /* bytes per second */
    PutU16(h + 32, (UINT16)block);
    PutU16(h + 34, 16);                      /* bits per sample */
    memcpy(h + 36, "data", 4);
    PutU32(h + 40, dataBytes);
}

WavSink *Wav_Create(const wchar_t *path, UINT32 rate, UINT32 channels)
{
    WavSink *w;
    BYTE     header[HEADER_BYTES];
    DWORD    wrote = 0;

    if (!path || !rate || !channels)
        return NULL;

    w = (WavSink *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w)
        return NULL;

    w->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w->file == INVALID_HANDLE_VALUE) {
        Log_Printf(L"wav: cannot create %s (%lu)", path, GetLastError());
        HeapFree(GetProcessHeap(), 0, w);
        return NULL;
    }
    StringCchCopyW(w->path, MAX_PATH, path);
    w->rate     = rate;
    w->channels = channels;

    /* Written again with the real sizes on close. */
    BuildHeader(header, rate, channels, 0);
    if (!WriteFile(w->file, header, HEADER_BYTES, &wrote, NULL) ||
        wrote != HEADER_BYTES) {
        CloseHandle(w->file);
        DeleteFileW(path);
        HeapFree(GetProcessHeap(), 0, w);
        return NULL;
    }
    Log_Printf(L"wav: sidecar open, %lu Hz, %u channel(s)",
               (unsigned long)rate, channels);
    return w;
}

BOOL Wav_Write(WavSink *w, const BYTE *pcm16, UINT32 frames)
{
    UINT32 bytes;
    DWORD  wrote = 0;
    BOOL   ok;

    if (!w || !frames)
        return FALSE;
    bytes = frames * w->channels * 2;

    if (w->full || bytes > WAV_MAX_DATA - w->dataBytes) {
        if (!w->full) {
            w->full = TRUE;
            Log_Printf(L"wav: 4 GB limit reached, sidecar stops here");
        }
        return FALSE;
    }

    if (pcm16) {
        ok = WriteFile(w->file, pcm16, bytes, &wrote, NULL) && wrote == bytes;
    } else {
        /* Padding. Written in chunks so a long gap does not need a big
           allocation. */
        static const BYTE zero[4096] = { 0 };
        UINT32 left = bytes;
        ok = TRUE;
        while (left && ok) {
            DWORD chunk = left > sizeof(zero) ? (DWORD)sizeof(zero) : left;
            ok = WriteFile(w->file, zero, chunk, &wrote, NULL) && wrote == chunk;
            left -= chunk;
        }
    }
    if (ok)
        w->dataBytes += bytes;
    return ok;
}

BOOL Wav_Close(WavSink *w)
{
    BYTE          header[HEADER_BYTES];
    LARGE_INTEGER zero;
    DWORD         wrote = 0;
    BOOL          ok = FALSE;

    if (!w)
        return FALSE;

    if (w->dataBytes == 0) {
        Log_Printf(L"wav: nothing was captured, removing the sidecar");
        CloseHandle(w->file);
        DeleteFileW(w->path);
        HeapFree(GetProcessHeap(), 0, w);
        return FALSE;
    }

    BuildHeader(header, w->rate, w->channels, w->dataBytes);
    zero.QuadPart = 0;
    if (SetFilePointerEx(w->file, zero, NULL, FILE_BEGIN) &&
        WriteFile(w->file, header, HEADER_BYTES, &wrote, NULL) &&
        wrote == HEADER_BYTES)
        ok = TRUE;

    CloseHandle(w->file);
    if (ok)
        Log_Printf(L"wav: sidecar closed, %.2f s of PCM",
                   (double)w->dataBytes / (double)(w->rate * w->channels * 2));
    else
        Log_Printf(L"wav: the header could not be finished");
    HeapFree(GetProcessHeap(), 0, w);
    return ok;
}
