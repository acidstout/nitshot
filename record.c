/*
 * record.c - the recording worker: duplication in, MP4 out.
 *
 * Two capture paths. The SDR one takes the tone-mapped 8-bit desktop from
 * plain DuplicateOutput and hands it to the encoder as RGB32, which is what it
 * has always done. The HDR one asks DuplicateOutput1 for scRGB half-float,
 * converts it to 10-bit BT.2020 PQ on the GPU (hdrvideo.c) and feeds P010 to
 * HEVC Main10. Which one runs is decided per recording, from the display's
 * state and what the encoder probe found.
 */

#define COBJMACROS

#include "nitshot.h"
#include "record.h"
#include "settings.h"
#include "encoder.h"
#include "hdrvideo.h"
#include "mp4tag.h"
#include "wavsink.h"
#include "remux.h"
#include "lang.h"
#include "hdr.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>      /* IDXGIOutput6, for the display's colour volume */
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <process.h>


/* How long the source must have been silent before the timeline is padded,
   and how much of the recent past is left alone while doing it. */
#define QUIET_BEFORE_PAD_MS  400
#define PAD_LAG_MS           250

/*
 * MFSetAttributeSize and MFSetAttributeRatio are inline helpers that the SDK
 * only defines for C++ - they call pAttributes->SetUINT64 directly. The packing
 * they do is no secret, so C does it here.
 */
static void SetPacked(IMFMediaType *t, const GUID *key, UINT32 high, UINT32 low)
{
    IMFMediaType_SetUINT64(t, key, ((UINT64)high << 32) | (UINT64)low);
}

typedef struct {
    /* configuration */
    RecordOptions opt;
    RECT          out;          /* region clipped to one output, screen coords */
    HWND          notify;
    UINT          doneMsg;

    /* threading */
    HANDLE        thread;
    HANDLE        stopEvent;
    volatile LONG active;
    CRITICAL_SECTION writeLock; /* the sink writer is not thread safe */
    LARGE_INTEGER qpcStart, qpcFreq;

    /* capture */
    ID3D11Device           *dev;
    ID3D11DeviceContext    *ctx;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D        *staging;     /* SDR path only: BGRA readback      */
    HdrVideo               *hv;          /* HDR path only: scRGB -> P010      */
    POINT                   outOrigin;   /* the output's top-left, screen coords */
    wchar_t                 outName[32]; /* GDI device name, for the HDR query */
    /*
     * These are two different questions. 'floatCapture' is how the desktop is
     * taken; 'hdr' is what is written. On an HDR display the capture is always
     * floating point, because plain DuplicateOutput does not hand back a
     * usable SDR image there - it skips the divide by SDR white, so everything
     * arrives three times too bright and clips. An 8-bit recording of an HDR
     * desktop is therefore float in, tone-mapped here, 8-bit out.
     */
    BOOL                    floatCapture;
    BOOL                    hdr;         /* ...and encoding 10-bit PQ         */
    BOOL                    hdrLocked;   /* ...and the file already says so   */
    float                   sdrWhiteNits;
    /* What the file has to say about itself afterwards: the display it was
       made on, and how bright the frames actually turned out. */
    Mp4ColourInfo           colour;

    /* encoding */
    IMFSinkWriter *writer;
    EncoderId      enc;
    DWORD          videoStream, audioStream;
    UINT32         width, height;
    LONGLONG       audioFrames;          /* the audio timeline, in samples */
    ULONGLONG      lastAudioTick;        /* when real audio last arrived    */
    UINT32         audioRate, audioChannels;
    BOOL           haveAudio;
    WavSink       *wav;                  /* the lossless sidecar, or NULL   */
    BOOL           muxFromWav;           /* no live AAC; build it afterwards */

    RecordResult  *result;
} Recorder;

static Recorder g_rec;

void Record_FreeResult(RecordResult *res)
{
    if (res)
        HeapFree(GetProcessHeap(), 0, res);
}

BOOL Record_IsActive(void)
{
    return InterlockedCompareExchange(&g_rec.active, 0, 0) != 0;
}

DWORD Record_ElapsedMs(void)
{
    LARGE_INTEGER now;

    if (!Record_IsActive() || g_rec.qpcFreq.QuadPart == 0)
        return 0;
    QueryPerformanceCounter(&now);
    return (DWORD)((now.QuadPart - g_rec.qpcStart.QuadPart) * 1000 /
                   g_rec.qpcFreq.QuadPart);
}

static LONGLONG ElapsedNs100(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_rec.qpcStart.QuadPart) * 10000000 /
           g_rec.qpcFreq.QuadPart;
}

static void Fail(const wchar_t *why)
{
    if (g_rec.result && !g_rec.result->error[0])
        StringCchCopyW(g_rec.result->error, ARRAYSIZE(g_rec.result->error), why);
    Log_Printf(L"record: %s", why);
}

/* --------------------------------------------------------- the duplication */

/* Find the output containing the region's centre, and bring up duplication for
   it. A region spanning two monitors is clipped to one; recording a composite
   of several outputs would need one duplication each and a shared clock. */
static BOOL OpenOutput(void)
{
    IDXGIFactory1 *factory = NULL;
    POINT          centre;
    BOOL           ok = FALSE;
    UINT           ai;

    centre.x = (g_rec.opt.region.left + g_rec.opt.region.right) / 2;
    centre.y = (g_rec.opt.region.top + g_rec.opt.region.bottom) / 2;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return FALSE;

    for (ai = 0; !ok; ai++) {
        IDXGIAdapter1 *adapter = NULL;
        UINT oi;

        if (IDXGIFactory1_EnumAdapters1(factory, ai, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        for (oi = 0; !ok; oi++) {
            IDXGIOutput      *output = NULL;
            IDXGIOutput1     *out1   = NULL;
            DXGI_OUTPUT_DESC  od;
            D3D_FEATURE_LEVEL got;

            if (IDXGIAdapter1_EnumOutputs(adapter, oi, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(IDXGIOutput_GetDesc(output, &od)) || !od.AttachedToDesktop ||
                !PtInRect(&od.DesktopCoordinates, centre)) {
                IDXGIOutput_Release(output);
                continue;
            }

            if (SUCCEEDED(D3D11CreateDevice((IDXGIAdapter *)adapter,
                                            D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                            NULL, 0, D3D11_SDK_VERSION,
                                            &g_rec.dev, &got, &g_rec.ctx)) &&
                SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1,
                                                     (void **)&out1))) {
                HRESULT hrDup = E_FAIL;

                StringCchCopyW(g_rec.outName, ARRAYSIZE(g_rec.outName),
                               od.DeviceName);

                /*
                 * DuplicateOutput1 with a float format is the only way to be
                 * handed the desktop before the compositor tone-maps it to
                 * 8-bit. Plain DuplicateOutput on an HDR display gives the
                 * already-flattened version - which is what every recording
                 * before this looked like.
                 */
                if (g_rec.floatCapture) {
                    IDXGIOutput5 *out5 = NULL;
                    if (SUCCEEDED(IDXGIOutput_QueryInterface(output,
                            &IID_IDXGIOutput5, (void **)&out5))) {
                        DXGI_FORMAT want = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        hrDup = IDXGIOutput5_DuplicateOutput1(out5,
                                    (IUnknown *)g_rec.dev, 0, 1, &want,
                                    &g_rec.dup);
                        if (FAILED(hrDup))
                            Log_Printf(L"record: DuplicateOutput1(FLOAT) failed "
                                       L"0x%08lX, dropping to 8-bit",
                                       (unsigned long)hrDup);
                        IDXGIOutput5_Release(out5);
                    }
                    /*
                     * Before writing starts, dropping to 8-bit is a fair
                     * trade. Once the file's media type says 10-bit BT.2020
                     * it is not: an 8-bit frame cannot be fed to it and the
                     * codec cannot change mid-stream, so a rebuild that
                     * cannot get float back reports failure and the loop
                     * retries, repeating the last frame meanwhile.
                     */
                    if (FAILED(hrDup)) {
                        if (g_rec.hdrLocked) {
                            IDXGIOutput1_Release(out1);
                            IDXGIOutput_Release(output);
                            IDXGIAdapter1_Release(adapter);
                            IDXGIFactory1_Release(factory);
                            return FALSE;
                        }
                        g_rec.floatCapture = FALSE;
                        g_rec.hdr          = FALSE;
                    }
                }

                if (FAILED(hrDup)) {
                    hrDup = IDXGIOutput1_DuplicateOutput(out1,
                                (IUnknown *)g_rec.dev, &g_rec.dup);
                    if (FAILED(hrDup))
                        Log_Printf(L"record: DuplicateOutput failed 0x%08lX",
                                   (unsigned long)hrDup);
                }

                /*
                 * The display's own colour volume is the honest answer to
                 * "what was this graded on", and without it a player has to
                 * guess - defensively, and at the cost of brightness.
                 */
                if (SUCCEEDED(hrDup) && g_rec.hdr) {
                    IDXGIOutput6 *out6 = NULL;
                    if (SUCCEEDED(IDXGIOutput_QueryInterface(output,
                            &IID_IDXGIOutput6, (void **)&out6))) {
                        DXGI_OUTPUT_DESC1 d1;
                        if (SUCCEEDED(IDXGIOutput6_GetDesc1(out6, &d1))) {
                            g_rec.colour.redX   = d1.RedPrimary[0];
                            g_rec.colour.redY   = d1.RedPrimary[1];
                            g_rec.colour.greenX = d1.GreenPrimary[0];
                            g_rec.colour.greenY = d1.GreenPrimary[1];
                            g_rec.colour.blueX  = d1.BluePrimary[0];
                            g_rec.colour.blueY  = d1.BluePrimary[1];
                            g_rec.colour.whiteX = d1.WhitePoint[0];
                            g_rec.colour.whiteY = d1.WhitePoint[1];
                            g_rec.colour.maxMasteringNits = d1.MaxLuminance;
                            g_rec.colour.minMasteringNits = d1.MinLuminance;
                            Log_Printf(L"record: mastering display %.0f..%.4f "
                                       L"nits, white %.4f,%.4f",
                                       d1.MaxLuminance, d1.MinLuminance,
                                       d1.WhitePoint[0], d1.WhitePoint[1]);
                        }
                        IDXGIOutput6_Release(out6);
                    }
                }

                if (SUCCEEDED(hrDup)) {
                    g_rec.outOrigin.x = od.DesktopCoordinates.left;
                    g_rec.outOrigin.y = od.DesktopCoordinates.top;
                    g_rec.out = g_rec.opt.region;
                    IntersectRect(&g_rec.out, &g_rec.out, &od.DesktopCoordinates);
                    ok = TRUE;
                }
                IDXGIOutput1_Release(out1);
            }
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory1_Release(factory);
    return ok;
}

/* SDR only: duplication delivers BGRA and the encoder takes RGB32, so the
   frame just needs to reach the CPU. The HDR path renders instead. */
static BOOL MakeStaging(void)
{
    D3D11_TEXTURE2D_DESC d;

    ZeroMemory(&d, sizeof(d));
    d.Width            = g_rec.width;
    d.Height           = g_rec.height;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_STAGING;
    d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    return SUCCEEDED(ID3D11Device_CreateTexture2D(g_rec.dev, &d, NULL, &g_rec.staging));
}

/* ------------------------------------------------------------ the encoder */

/*
 * Which encoder, and therefore which capture path. The probe in encoder.c has
 * already established what this machine can actually finalise a file with, so
 * all that is left here is to honour the setting and record what was chosen.
 */
static BOOL ChooseEncoder(void)
{
    EncoderId    want = Encoder_FromToken(g_cfg.recordEncoder);
    HdrInfo      info;
    BOOL         displayHdr = FALSE;
    MONITORINFOEXW mi;
    POINT        centre;
    HMONITOR     mon;

    /* The monitor is looked up here rather than taken from OpenOutput, because
       the answer decides which duplication format OpenOutput has to ask for. */
    centre.x = (g_rec.opt.region.left + g_rec.opt.region.right) / 2;
    centre.y = (g_rec.opt.region.top + g_rec.opt.region.bottom) / 2;
    mon = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, (MONITORINFO *)&mi))
        StringCchCopyW(g_rec.outName, ARRAYSIZE(g_rec.outName), mi.szDevice);

    g_rec.sdrWhiteNits = 80.0f;
    if (!g_cfg.forceSdr && g_rec.outName[0] &&
        Hdr_QueryMonitor(g_rec.outName, &info) && info.active) {
        displayHdr = TRUE;
        g_rec.sdrWhiteNits = info.sdrWhiteNits;
        /* Float capture regardless of what gets written; the 8-bit desktop
           duplication is simply wrong on such a display. */
        g_rec.floatCapture = TRUE;
    }
    if (!g_cfg.recordHdr)
        displayHdr = FALSE;      /* capture float, but write 8-bit */

    g_rec.enc = Encoder_Resolve(want, displayHdr);
    if (g_rec.enc >= ENC_COUNT) {
        Log_Printf(L"record: no usable video encoder at all");
        return FALSE;
    }
    g_rec.hdr = Encoder_IsHdr(g_rec.enc);

    Log_Printf(L"record: display is %s, using %s (%s)",
               displayHdr ? L"HDR" : L"SDR", Encoder_Name(g_rec.enc),
               Encoder_Caps()->mft[g_rec.enc][0]
                   ? Encoder_Caps()->mft[g_rec.enc] : L"unnamed");
    return TRUE;
}

static BOOL AddAudioStream(UINT32 rate, UINT32 channels)
{
    IMFMediaType *outType = NULL, *inType = NULL;
    DWORD         index = 0;
    BOOL          ok = FALSE;

    if (FAILED(MFCreateMediaType(&outType)))
        goto done;
    IMFMediaType_SetGUID(outType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(outType, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    IMFMediaType_SetUINT32(outType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    if (FAILED(IMFSinkWriter_AddStream(g_rec.writer, outType, &index)))
        goto done;
    g_rec.audioStream = index;

    if (FAILED(MFCreateMediaType(&inType)))
        goto done;
    IMFMediaType_SetGUID(inType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(inType, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    IMFMediaType_SetUINT32(inType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           rate * channels * 2);
    if (FAILED(IMFSinkWriter_SetInputMediaType(g_rec.writer, g_rec.audioStream,
                                               inType, NULL)))
        goto done;
    ok = TRUE;

done:
    if (inType)  IMFMediaType_Release(inType);
    if (outType) IMFMediaType_Release(outType);
    return ok;
}

static BOOL PickPath(wchar_t *path, size_t cch)
{
    wchar_t    dir[MAX_PATH];
    SYSTEMTIME st;
    int        n;

    if (!Settings_ResolveVideoFolder(dir, ARRAYSIZE(dir)))
        return FALSE;

    GetLocalTime(&st);
    for (n = 1; n <= 99; n++) {
        HRESULT hr;
        if (n == 1)
            hr = StringCchPrintfW(path, cch,
                    L"%s\\Recording %04u-%02u-%02u %02u%02u%02u.mp4",
                    dir, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
        else
            hr = StringCchPrintfW(path, cch,
                    L"%s\\Recording %04u-%02u-%02u %02u%02u%02u (%d).mp4",
                    dir, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, n);
        if (FAILED(hr))
            return FALSE;
        if (!PathFileExistsW(path))
            return TRUE;
    }
    return FALSE;
}

static BOOL OpenWriter(void)
{
    IMFAttributes *attrs = NULL;
    BOOL ok = FALSE;

    if (!PickPath(g_rec.result->path, ARRAYSIZE(g_rec.result->path)))
        return FALSE;

    if (FAILED(MFCreateAttributes(&attrs, 3)))
        return FALSE;
    IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                          &MFTranscodeContainerType_MPEG4);
    IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    /* Without this the writer blocks the capture loop to pace the encoder,
       which for a live screen recording means dropped frames instead. */
    IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    if (SUCCEEDED(MFCreateSinkWriterFromURL(g_rec.result->path, NULL, attrs,
                                            &g_rec.writer)))
        ok = TRUE;

    IMFAttributes_Release(attrs);
    return ok;
}

/* ------------------------------------------------------------- the samples */

/* Wraps a filled buffer as a timed sample and hands it to the writer. */
static void SubmitVideo(IMFMediaBuffer *buf, DWORD size, LONGLONG when)
{
    IMFSample *sample = NULL;

    IMFMediaBuffer_SetCurrentLength(buf, size);
    if (FAILED(MFCreateSample(&sample)))
        return;
    IMFSample_AddBuffer(sample, buf);
    IMFSample_SetSampleTime(sample, when);
    IMFSample_SetSampleDuration(sample, 10000000LL / g_rec.opt.fps);

    EnterCriticalSection(&g_rec.writeLock);
    IMFSinkWriter_WriteSample(g_rec.writer, g_rec.videoStream, sample);
    LeaveCriticalSection(&g_rec.writeLock);
    IMFSample_Release(sample);
}

/* SDR: the staging texture's rows, stripped of their padding. */
static void WriteRgb32Sample(const BYTE *bgra, UINT32 stride, LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD           size = g_rec.width * 4 * g_rec.height;
    BYTE           *dst = NULL;

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        UINT32 y;
        for (y = 0; y < g_rec.height; y++)
            memcpy(dst + (SIZE_T)y * g_rec.width * 4,
                   bgra + (SIZE_T)y * stride, (SIZE_T)g_rec.width * 4);
        IMFMediaBuffer_Unlock(buf);
        SubmitVideo(buf, size, when);
    }
    IMFMediaBuffer_Release(buf);
}

/* An HDR desktop recorded as 8-bit: float in, tone-mapped on the GPU, RGB32
   out, so the recording matches what the screen actually looked like. */
static void WriteToneMappedSample(LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD           size = g_rec.width * 4 * g_rec.height;
    BYTE           *dst = NULL;

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        BOOL ok = HdrVideo_ConvertToBgra(g_rec.hv, dst, g_rec.width * 4);
        IMFMediaBuffer_Unlock(buf);
        if (ok)
            SubmitVideo(buf, size, when);
    }
    IMFMediaBuffer_Release(buf);
}

/* HDR: the conversion writes P010 straight into the encoder's buffer. */
static void WriteP010Sample(LONGLONG when)
{
    IMFMediaBuffer *buf = NULL;
    DWORD           size = g_rec.width * g_rec.height * 3;
    BYTE           *dst = NULL;

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        BOOL ok = HdrVideo_ConvertToP010(g_rec.hv, dst);
        IMFMediaBuffer_Unlock(buf);
        if (ok)
            SubmitVideo(buf, size, when);
    }
    IMFMediaBuffer_Release(buf);
}

/* Write one PCM block at the current point on the audio timeline. */
static void WriteAudioBlock(const BYTE *pcm16, UINT32 frames)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *sample = NULL;
    DWORD           size = frames * g_rec.audioChannels * 2;
    BYTE           *dst = NULL;

    if (size == 0)
        return;

    /* No AAC stream exists in this mode; the WAV is the only destination, and
       the padding still goes through here so the timeline stays right. */
    if (g_rec.muxFromWav) {
        if (g_rec.wav)
            Wav_Write(g_rec.wav, pcm16, frames);
        g_rec.audioFrames += frames;
        return;
    }

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        if (pcm16)
            memcpy(dst, pcm16, size);
        else
            ZeroMemory(dst, size);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, size);

        if (SUCCEEDED(MFCreateSample(&sample))) {
            IMFSample_AddBuffer(sample, buf);
            IMFSample_SetSampleTime(sample,
                g_rec.audioFrames * 10000000LL / g_rec.audioRate);
            IMFSample_SetSampleDuration(sample,
                (LONGLONG)frames * 10000000LL / g_rec.audioRate);
            IMFSinkWriter_WriteSample(g_rec.writer, g_rec.audioStream, sample);
            IMFSample_Release(sample);
        }
        /* Fed from here rather than from OnAudio so the sidecar gets the
           padding too and stays aligned with the video timeline. */
        if (g_rec.wav)
            Wav_Write(g_rec.wav, pcm16, frames);
        g_rec.audioFrames += frames;
    }
    IMFMediaBuffer_Release(buf);
}

/*
 * A loopback capture delivers nothing at all while the machine is silent - not
 * zeroed packets, no packets. Left alone that produces a recording with an
 * empty audio track, or one whose sound jumps whenever playback paused. The
 * video loop therefore tops the audio timeline up with real silence whenever it
 * falls behind the elapsed time.
 */
static void PadAudioSilence(void)
{
    LONGLONG expected, behind;
    UINT32   chunk;

    if (!g_rec.haveAudio || g_rec.audioRate == 0)
        return;

    /*
     * Only when the source has genuinely gone quiet. Comparing the audio
     * timeline against the clock is not enough on its own: capture always runs
     * a little behind the clock, so a threshold small enough to catch real
     * gaps also fires constantly during normal playback - and every spurious
     * pad inserts silence in the middle of continuous sound and pushes the
     * samples already in flight to a later timestamp. That was audible.
     */
    if (GetTickCount64() - g_rec.lastAudioTick < QUIET_BEFORE_PAD_MS)
        return;

    EnterCriticalSection(&g_rec.writeLock);
    /* Never fill the most recent stretch: samples for it may still be on
       their way from the audio engine. */
    expected = (ElapsedNs100() - PAD_LAG_MS * 10000LL) * g_rec.audioRate / 10000000LL;
    behind   = expected - g_rec.audioFrames;
    if (behind > 0) {
        while (behind > 0) {
            chunk = (UINT32)(behind > 4096 ? 4096 : behind);
            WriteAudioBlock(NULL, chunk);
            behind -= chunk;
        }
    }
    LeaveCriticalSection(&g_rec.writeLock);
}

/*
 * Audio is timestamped from its own sample count rather than the wall clock.
 * The endpoint's clock and QPC drift apart slightly, and a timeline built from
 * the samples actually captured is the one that plays back without gaps.
 */
static void OnAudio(void *ctx, const BYTE *pcm16, UINT32 frames)
{
    UNREFERENCED_PARAMETER(ctx);
    if (!g_rec.writer || !g_rec.haveAudio || !Record_IsActive())
        return;

    EnterCriticalSection(&g_rec.writeLock);
    g_rec.lastAudioTick = GetTickCount64();
    WriteAudioBlock(pcm16, frames);
    LeaveCriticalSection(&g_rec.writeLock);
}

/* ---------------------------------------------------------------- the loop */

/* Pull the newest frame, if any, into the staging texture. */
static BOOL GrabFrame(void)
{
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource   *res = NULL;
    ID3D11Texture2D *tex = NULL;
    D3D11_BOX        box;
    HRESULT          hr;
    BOOL             got = FALSE;

    hr = IDXGIOutputDuplication_AcquireNextFrame(g_rec.dup, 0, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return FALSE;
    if (FAILED(hr)) {
        /* The mode changed or another process took exclusive control; a fresh
           duplication object is the documented recovery. */
        IDXGIOutputDuplication_Release(g_rec.dup);
        g_rec.dup = NULL;
        return FALSE;
    }

    /*
     * A frame that only reports the pointer moving carries no new image, and
     * on a duplication that has not presented yet its surface is empty -
     * copying it would start the recording on black frames. The copy of the
     * last real image is still in the capture texture, so there is nothing to
     * do but let this one go.
     */
    if (info.LastPresentTime.QuadPart == 0) {
        IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(g_rec.dup);
        return FALSE;
    }

    if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D,
                                               (void **)&tex))) {
        ID3D11Texture2D *dest = g_rec.floatCapture ? HdrVideo_Input(g_rec.hv)
                                                   : g_rec.staging;
        if (dest) {
            box.left   = (UINT)(g_rec.out.left   - g_rec.outOrigin.x);
            box.top    = (UINT)(g_rec.out.top    - g_rec.outOrigin.y);
            box.right  = box.left + g_rec.width;
            box.bottom = box.top  + g_rec.height;
            box.front  = 0;
            box.back   = 1;
            ID3D11DeviceContext_CopySubresourceRegion(g_rec.ctx,
                (ID3D11Resource *)dest, 0, 0, 0, 0,
                (ID3D11Resource *)tex, 0, &box);
            got = TRUE;
        }
        ID3D11Texture2D_Release(tex);
    }
    IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(g_rec.dup);
    return got;
}

static unsigned __stdcall RecordThread(void *arg)
{
    AudioFormat afmt;
    LONGLONG    frameIndex = 0;
    DWORD       intervalMs;
    BOOL        haveFrame = FALSE;

    UNREFERENCED_PARAMETER(arg);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    intervalMs = (DWORD)(1000 / g_rec.opt.fps);

    if (!ChooseEncoder())         { Fail(Lang_Str(STR_ERR_NO_ENCODER)); goto done; }
    if (!OpenOutput())            { Fail(Lang_Str(STR_ERR_NO_DISPLAY)); goto done; }

    /* Every codec here wants even dimensions, and 4:2:0 chroma has no meaning
       without them. */
    g_rec.width  = (UINT32)(g_rec.out.right - g_rec.out.left) & ~1u;
    g_rec.height = (UINT32)(g_rec.out.bottom - g_rec.out.top) & ~1u;
    if (g_rec.width < 16 || g_rec.height < 16) { Fail(Lang_Str(STR_ERR_AREA_SMALL)); goto done; }

    /*
     * OpenOutput may have had to fall back to 8-bit duplication, in which case
     * the 10-bit encoder chosen a moment ago no longer matches what is being
     * captured and has to be reconsidered.
     */
    if (!g_rec.hdr && Encoder_IsHdr(g_rec.enc)) {
        EncoderId next = Encoder_Resolve(Encoder_FromToken(g_cfg.recordEncoder), FALSE);
        if (next >= ENC_COUNT || Encoder_IsHdr(next))
            next = Encoder_Fallback(g_rec.enc);
        if (next >= ENC_COUNT)    { Fail(Lang_Str(STR_ERR_NO_ENCODER)); goto done; }
        Log_Printf(L"record: no float duplication, falling back to %s",
                   Encoder_Name(next));
        g_rec.enc = next;
    }

    if (g_rec.floatCapture) {
        g_rec.hv = HdrVideo_Create(g_rec.dev, g_rec.ctx, g_rec.width, g_rec.height);
        if (!g_rec.hv) { Fail(Lang_Str(STR_ERR_HDR_SETUP)); goto done; }
        /* Only the 8-bit path uses these, but setting them is free. */
        HdrVideo_SetSdrWhite(g_rec.hv, g_rec.sdrWhiteNits, g_cfg.hdrSdrRollOff);
        if (!g_rec.hdr)
            Log_Printf(L"record: float capture, tone-mapped to 8-bit at "
                       L"%.0f nits white%s", g_rec.sdrWhiteNits,
                       g_cfg.hdrSdrRollOff ? L", highlights rolled off" : L"");
    } else if (!MakeStaging()) {
        Fail(Lang_Str(STR_ERR_CAPTURE_BUF));
        goto done;
    }

    if (!OpenWriter())            { Fail(Lang_Str(STR_ERR_FILE_CREATE)); goto done; }
    if (!Encoder_AddVideoStream(g_rec.writer, g_rec.enc, g_rec.width,
                                g_rec.height, g_rec.opt.fps,
                                &g_rec.videoStream)) {
        Fail(Lang_Str(STR_ERR_ENC_FORMAT));
        goto done;
    }

    ZeroMemory(&afmt, sizeof(afmt));
    if (g_rec.opt.audio != AUDIO_NONE) {
        /* Only meaningful with a sidecar to build the sound from. */
        g_rec.muxFromWav = g_cfg.recordWavSidecar && g_cfg.recordMuxFromWav;

        if (Audio_Start(g_rec.opt.audio, &afmt, OnAudio, NULL)) {
            /*
             * With the sidecar as the source of the MP4's sound there is no
             * AAC stream in the file while recording - the samples go to the
             * WAV only, and the encoding happens once, afterwards. Nothing
             * then has to keep up with the capture clock.
             */
            BOOL haveStream = g_rec.muxFromWav ||
                              AddAudioStream(afmt.sampleRate, afmt.channels);

            if (haveStream) {
                g_rec.audioRate     = afmt.sampleRate;
                g_rec.audioChannels = afmt.channels;
                g_rec.haveAudio     = TRUE;

                if (g_cfg.recordWavSidecar) {
                    StringCchCopyW(g_rec.result->wavPath,
                                   ARRAYSIZE(g_rec.result->wavPath),
                                   g_rec.result->path);
                    PathRenameExtensionW(g_rec.result->wavPath, L".wav");
                    g_rec.wav = Wav_Create(g_rec.result->wavPath,
                                           afmt.sampleRate, afmt.channels);
                    if (!g_rec.wav)
                        g_rec.result->wavPath[0] = 0;
                }
                /* No sidecar means nothing to mux from later, so the live AAC
                   stream has to be there after all. */
                if (g_rec.muxFromWav && !g_rec.wav) {
                    Log_Printf(L"record: no sidecar, encoding AAC live instead");
                    g_rec.muxFromWav = FALSE;
                    if (!AddAudioStream(afmt.sampleRate, afmt.channels)) {
                        Audio_Stop();
                        g_rec.haveAudio = FALSE;
                        Log_Printf(L"record: no AAC encoder, continuing silent");
                    }
                }
            } else {
                Audio_Stop();
                Log_Printf(L"record: no AAC encoder, continuing without sound");
            }
        } else {
            Log_Printf(L"record: audio could not be opened, continuing silent");
        }
    }

    if (FAILED(IMFSinkWriter_BeginWriting(g_rec.writer))) {
        Fail(Lang_Str(STR_ERR_ENC_START));
        goto done;
    }

    QueryPerformanceFrequency(&g_rec.qpcFreq);
    QueryPerformanceCounter(&g_rec.qpcStart);
    g_rec.lastAudioTick = GetTickCount64();
    g_rec.hdrLocked     = g_rec.hdr;
    InterlockedExchange(&g_rec.active, 1);

    for (;;) {
        LONGLONG due;
        LONGLONG now;

        if (g_rec.dup && GrabFrame())
            haveFrame = TRUE;

        /*
         * Every tick writes a frame, repeating the last one when nothing
         * changed. A still desktop produces no duplication frames at all, and
         * a video whose timeline simply stops for those seconds plays back
         * wrongly in most players.
         */
        if (haveFrame) {
            LONGLONG when = frameIndex * 10000000LL / g_rec.opt.fps;
            if (g_rec.hdr) {
                double peak = 0.0, mean = 0.0;
                WriteP010Sample(when);
                /* MaxCLL is the brightest pixel anywhere in the recording,
                   MaxFALL the brightest frame average - both maxima, so they
                   only ever climb. */
                HdrVideo_LastFrameLight(g_rec.hv, &peak, &mean);
                if (peak > g_rec.colour.maxCLL)
                    g_rec.colour.maxCLL = (float)peak;
                if (mean > g_rec.colour.maxFALL)
                    g_rec.colour.maxFALL = (float)mean;
            } else if (g_rec.floatCapture) {
                WriteToneMappedSample(when);
            } else {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(ID3D11DeviceContext_Map(g_rec.ctx,
                        (ID3D11Resource *)g_rec.staging, 0, D3D11_MAP_READ, 0,
                        &map))) {
                    WriteRgb32Sample((const BYTE *)map.pData, map.RowPitch, when);
                    ID3D11DeviceContext_Unmap(g_rec.ctx,
                                              (ID3D11Resource *)g_rec.staging, 0);
                }
            }
        }
        frameIndex++;
        PadAudioSilence();

        /* Pace against the start, so a slow tick does not push every later
           frame late and stretch the recording. */
        due = frameIndex * 1000LL / g_rec.opt.fps;
        now = ElapsedNs100() / 10000;
        {
            DWORD waitMs = due > now ? (DWORD)(due - now) : 0;
            if (waitMs > intervalMs * 4)
                waitMs = intervalMs * 4;
            if (WaitForSingleObject(g_rec.stopEvent, waitMs) == WAIT_OBJECT_0)
                break;
        }

        if (!g_rec.dup) {
            /* Rebuild after a lost duplication; the file keeps running. */
            if (!OpenOutput())
                Sleep(intervalMs);
        }
    }

    g_rec.result->seconds = Record_ElapsedMs() / 1000;
    InterlockedExchange(&g_rec.active, 0);

    Audio_Stop();
    EnterCriticalSection(&g_rec.writeLock);
    g_rec.result->ok = SUCCEEDED(IMFSinkWriter_Finalize(g_rec.writer));
    if (g_rec.wav) {
        if (!Wav_Close(g_rec.wav))
            g_rec.result->wavPath[0] = 0;
        g_rec.wav = NULL;
    }
    LeaveCriticalSection(&g_rec.writeLock);
    if (!g_rec.result->ok) {
        Fail(Lang_Str(STR_ERR_FINALISE));
    } else {
        /*
         * The sound is built from the sidecar here, before the colour box is
         * stamped: remuxing writes a brand new file, so anything added to the
         * old one would be thrown away.
         */
        if (g_rec.muxFromWav && g_rec.result->wavPath[0]) {
            if (Remux_AddWavAudio(g_rec.result->path, g_rec.result->wavPath)) {
                /*
                 * In this mode the WAV was only ever the way the sound reached
                 * the encoder. Once it is in the MP4, keeping it as well just
                 * leaves two copies of the same audio in the folder.
                 */
                if (DeleteFileW(g_rec.result->wavPath))
                    g_rec.result->wavPath[0] = 0;
                else
                    Log_Printf(L"record: the sidecar could not be removed (%lu)",
                               GetLastError());
            } else {
                /* Keeping it is the whole reason the recording still has its
                   sound at all. */
                Log_Printf(L"record: the sound could not be muxed in; the video "
                           L"is silent and the sound is in the .wav");
            }
        }

        if (g_rec.hdr) {
            /* 9 = BT.2020 primaries, 16 = SMPTE ST 2084, 9 = BT.2020 NCL, and
               the range is limited because that is what the encoder was given. */
            g_rec.colour.primaries = 9;
            g_rec.colour.transfer  = 16;
            g_rec.colour.matrix    = 9;
            g_rec.colour.fullRange = FALSE;
            Mp4Tag_WriteColour(g_rec.result->path, &g_rec.colour);
        }
    }

done:
    InterlockedExchange(&g_rec.active, 0);
    Audio_Stop();
    /* Only reached with a sink still open when setup failed part-way. */
    if (g_rec.wav) {
        Wav_Close(g_rec.wav);
        g_rec.wav = NULL;
    }

    if (g_rec.writer)  { IMFSinkWriter_Release(g_rec.writer);          g_rec.writer  = NULL; }
    if (g_rec.hv)      { HdrVideo_Destroy(g_rec.hv);                   g_rec.hv      = NULL; }
    if (g_rec.staging) { ID3D11Texture2D_Release(g_rec.staging);       g_rec.staging = NULL; }
    if (g_rec.dup)     { IDXGIOutputDuplication_Release(g_rec.dup);    g_rec.dup     = NULL; }
    if (g_rec.ctx)     { ID3D11DeviceContext_Release(g_rec.ctx);       g_rec.ctx     = NULL; }
    if (g_rec.dev)     { ID3D11Device_Release(g_rec.dev);              g_rec.dev     = NULL; }

    MFShutdown();
    CoUninitialize();

    {
        RecordResult *res = g_rec.result;
        g_rec.result = NULL;
        if (res && !res->ok) {               /* a broken stub helps nobody */
            if (res->path[0])    DeleteFileW(res->path);
            if (res->wavPath[0]) DeleteFileW(res->wavPath);
        }
        if (res && !PostMessageW(g_rec.notify, g_rec.doneMsg, 0, (LPARAM)res))
            Record_FreeResult(res);
    }
    return 0;
}

/* --------------------------------------------------------------- the entry */

BOOL Record_Start(HWND notify, UINT doneMsg, const RecordOptions *opt)
{
    uintptr_t th;

    if (!opt || Record_IsActive())
        return FALSE;

    ZeroMemory(&g_rec, sizeof(g_rec));
    g_rec.opt     = *opt;
    g_rec.notify  = notify;
    g_rec.doneMsg = doneMsg;
    if (g_rec.opt.fps < 5)  g_rec.opt.fps = 5;
    if (g_rec.opt.fps > 60) g_rec.opt.fps = 60;

    g_rec.result = (RecordResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             sizeof(RecordResult));
    if (!g_rec.result)
        return FALSE;

    InitializeCriticalSection(&g_rec.writeLock);
    g_rec.stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_rec.stopEvent) {
        DeleteCriticalSection(&g_rec.writeLock);
        Record_FreeResult(g_rec.result);
        g_rec.result = NULL;
        return FALSE;
    }

    th = _beginthreadex(NULL, 0, RecordThread, NULL, 0, NULL);
    if (!th) {
        CloseHandle(g_rec.stopEvent);
        g_rec.stopEvent = NULL;
        DeleteCriticalSection(&g_rec.writeLock);
        Record_FreeResult(g_rec.result);
        g_rec.result = NULL;
        return FALSE;
    }
    g_rec.thread = (HANDLE)th;
    return TRUE;
}

void Record_Stop(void)
{
    if (g_rec.stopEvent)
        SetEvent(g_rec.stopEvent);
    if (g_rec.thread) {
        /* Waiting keeps the "it is saved" message honest: MP4 finalisation
           writes the index, without which the file will not play. */
        WaitForSingleObject(g_rec.thread, 15000);
        CloseHandle(g_rec.thread);
        g_rec.thread = NULL;
    }
    if (g_rec.stopEvent) {
        CloseHandle(g_rec.stopEvent);
        g_rec.stopEvent = NULL;
        DeleteCriticalSection(&g_rec.writeLock);
    }
}
