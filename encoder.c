/*
 * encoder.c - probe-backed encoder selection for the recorder.
 */

#define COBJMACROS

#include "nitshot.h"
#include "encoder.h"
#include "settings.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <process.h>

/* The probe writes a frame at this size; encoders that refuse odd or tiny
   dimensions are not interesting, and 1280x720 is cheap. */
#define PROBE_W   1280
#define PROBE_H   720
#define PROBE_FPS 30

static EncoderCaps g_caps;
static LONG        g_probing;
/* The warm-up thread and a settings dialog can both ask at once, and they
   would otherwise be filling g_caps.mft[][] at the same time. */
static SRWLOCK     g_lock = SRWLOCK_INIT;

/* ------------------------------------------------------------------ names */

const wchar_t *Encoder_Name(EncoderId id)
{
    switch (id) {
    case ENC_AUTO:   return L"Automatic (best available)";
    case ENC_H264:   return L"H.264, 8-bit";
    case ENC_HEVC8:  return L"HEVC, 8-bit";
    case ENC_HEVC10: return L"HEVC, 10-bit HDR";
    default:         return L"(none)";
    }
}

const wchar_t *Encoder_Token(EncoderId id)
{
    switch (id) {
    case ENC_H264:   return L"h264";
    case ENC_HEVC8:  return L"hevc8";
    case ENC_HEVC10: return L"hevc10";
    default:         return L"auto";
    }
}

EncoderId Encoder_FromToken(const wchar_t *token)
{
    if (!token || !token[0])            return ENC_AUTO;
    if (!lstrcmpiW(token, L"h264"))     return ENC_H264;
    if (!lstrcmpiW(token, L"hevc8"))    return ENC_HEVC8;
    if (!lstrcmpiW(token, L"hevc10"))   return ENC_HEVC10;
    return ENC_AUTO;
}

BOOL Encoder_IsHdr(EncoderId id)
{
    return id == ENC_HEVC10;
}

const EncoderCaps *Encoder_Caps(void)
{
    return &g_caps;
}

/* ---------------------------------------------------------- stream set-up */

static void SetPacked(IMFMediaType *t, const GUID *key, UINT32 high, UINT32 low)
{
    IMFMediaType_SetUINT64(t, key, ((UINT64)high << 32) | (UINT64)low);
}

/*
 * Screen content is mostly flat with hard edges, which wants more bits than
 * camera footage of the same size. HEVC buys roughly a third at equal quality,
 * so it is given less rather than being allowed to look better by using the
 * same budget - a recording that is smaller for no visible loss is the point.
 */
static UINT32 PickBitrate(EncoderId id, UINT32 w, UINT32 h, int fps)
{
    double bits = (double)w * h * fps * 0.12;

    if (id == ENC_HEVC8 || id == ENC_HEVC10)
        bits *= 0.65;
    /* 10-bit HDR carries more than it looks: PQ puts real detail in the top
       stops that 8-bit never had, and starving it there is very visible. */
    if (id == ENC_HEVC10)
        bits *= 1.35;

    if (bits <  2000000.0) bits =  2000000.0;
    if (bits > 40000000.0) bits = 40000000.0;
    return (UINT32)bits;
}

BOOL Encoder_AddVideoStream(IMFSinkWriter *writer, EncoderId id,
                            UINT32 width, UINT32 height, int fps,
                            DWORD *streamIndex)
{
    IMFMediaType *outType = NULL, *inType = NULL;
    const BOOL    hdr = Encoder_IsHdr(id);
    DWORD         index = 0;
    BOOL          ok = FALSE;

    if (!writer || id <= ENC_AUTO || id >= ENC_COUNT)
        return FALSE;

    if (FAILED(MFCreateMediaType(&outType)))
        goto done;
    IMFMediaType_SetGUID(outType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(outType, &MF_MT_SUBTYPE,
                         id == ENC_H264 ? &MFVideoFormat_H264 : &MFVideoFormat_HEVC);
    IMFMediaType_SetUINT32(outType, &MF_MT_AVG_BITRATE,
                           PickBitrate(id, width, height, fps));
    IMFMediaType_SetUINT32(outType, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
    if (hdr) {
        IMFMediaType_SetUINT32(outType, &MF_MT_MPEG2_PROFILE,
                               eAVEncH265VProfile_Main_420_10);
        /* Without these the file decodes but every player guesses, and a PQ
           signal read as Rec.709 is exactly the washed-out look this whole
           change is about. */
        IMFMediaType_SetUINT32(outType, &MF_MT_VIDEO_PRIMARIES,
                               MFVideoPrimaries_BT2020);
        IMFMediaType_SetUINT32(outType, &MF_MT_TRANSFER_FUNCTION,
                               MFVideoTransFunc_2084);
        IMFMediaType_SetUINT32(outType, &MF_MT_YUV_MATRIX,
                               MFVideoTransferMatrix_BT2020_10);
        IMFMediaType_SetUINT32(outType, &MF_MT_VIDEO_NOMINAL_RANGE,
                               MFNominalRange_16_235);
    }
    SetPacked(outType, &MF_MT_FRAME_SIZE, width, height);
    SetPacked(outType, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    SetPacked(outType, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(IMFSinkWriter_AddStream(writer, outType, &index)))
        goto done;

    if (FAILED(MFCreateMediaType(&inType)))
        goto done;
    IMFMediaType_SetGUID(inType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetUINT32(inType, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
    if (hdr) {
        IMFMediaType_SetGUID(inType, &MF_MT_SUBTYPE, &MFVideoFormat_P010);
        IMFMediaType_SetUINT32(inType, &MF_MT_VIDEO_PRIMARIES,
                               MFVideoPrimaries_BT2020);
        IMFMediaType_SetUINT32(inType, &MF_MT_TRANSFER_FUNCTION,
                               MFVideoTransFunc_2084);
        IMFMediaType_SetUINT32(inType, &MF_MT_VIDEO_NOMINAL_RANGE,
                               MFNominalRange_16_235);
    } else {
        /* RGB32 straight in. It costs a video-processor MFT in front of the
           encoder, but duplication already hands over BGRA and converting it
           ourselves would buy one transform for a whole colour path. */
        IMFMediaType_SetGUID(inType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
        IMFMediaType_SetUINT32(inType, &MF_MT_DEFAULT_STRIDE, width * 4);
    }
    SetPacked(inType, &MF_MT_FRAME_SIZE, width, height);
    SetPacked(inType, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    SetPacked(inType, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(IMFSinkWriter_SetInputMediaType(writer, index, inType, NULL)))
        goto done;
    ok = TRUE;

done:
    if (streamIndex) *streamIndex = index;
    if (inType)  IMFMediaType_Release(inType);
    if (outType) IMFMediaType_Release(outType);
    return ok;
}

/* ------------------------------------------------------------- the probe */

/* Names the encoder MFT the writer settled on, so the log and the settings
   list say "NVIDIA HEVC Encoder MFT" rather than just "HEVC". */
static void NameTransform(IMFSinkWriter *writer, DWORD stream, EncoderId id)
{
    IMFSinkWriterEx *wx = NULL;
    IMFTransform    *mft = NULL;
    DWORD            t;

    if (FAILED(IMFSinkWriter_QueryInterface(writer, &IID_IMFSinkWriterEx,
                                            (void **)&wx)))
        return;
    for (t = 0; t < 8; t++) {
        GUID           cat;
        IMFAttributes *attrs = NULL;

        if (FAILED(IMFSinkWriterEx_GetTransformForStream(wx, stream, t, &cat, &mft)))
            break;
        if (SUCCEEDED(IMFTransform_GetAttributes(mft, &attrs))) {
            LPWSTR name = NULL;
            UINT32 len = 0, async = 0;
            IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &async);
            if (SUCCEEDED(IMFAttributes_GetAllocatedString(attrs,
                    &MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
                /* The video processor is unnamed, the encoder is not, so the
                   last named transform in the chain is the encoder. */
                StringCchCopyW(g_caps.mft[id], ARRAYSIZE(g_caps.mft[id]), name);
                g_caps.hardware[id] = async != 0;
                CoTaskMemFree(name);
            }
            IMFAttributes_Release(attrs);
        }
        IMFTransform_Release(mft);
        mft = NULL;
    }
    IMFSinkWriterEx_Release(wx);
}

/* One frame of mid-grey in whatever layout this encoder takes. */
static BOOL PushProbeFrame(IMFSinkWriter *writer, DWORD stream, EncoderId id)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample      *sample = NULL;
    DWORD           size;
    BYTE           *p = NULL;
    BOOL            ok = FALSE;

    if (Encoder_IsHdr(id))
        size = PROBE_W * PROBE_H * 3;               /* P010: 2 B Y + 1 B UV */
    else
        size = PROBE_W * PROBE_H * 4;               /* RGB32 */

    if (FAILED(MFCreateMemoryBuffer(size, &buf)))
        return FALSE;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
        FillMemory(p, size, 0x80);
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_SetCurrentLength(buf, size);

    if (SUCCEEDED(MFCreateSample(&sample))) {
        IMFSample_AddBuffer(sample, buf);
        IMFSample_SetSampleTime(sample, 0);
        IMFSample_SetSampleDuration(sample, 10000000LL / PROBE_FPS);
        ok = SUCCEEDED(IMFSinkWriter_WriteSample(writer, stream, sample));
        IMFSample_Release(sample);
    }
    IMFMediaBuffer_Release(buf);
    return ok;
}

/*
 * The whole point: configure exactly as recording would, write a frame and
 * finalise. Anything short of finalising proves nothing - AV1 gets all the way
 * to the last step before the MP4 sink admits it cannot mux it.
 */
static BOOL ProbeOne(EncoderId id, const wchar_t *dir)
{
    IMFSinkWriter *writer = NULL;
    IMFAttributes *attrs = NULL;
    wchar_t        path[MAX_PATH];
    DWORD          stream = 0;
    HRESULT        hr = E_FAIL;
    BOOL           ok = FALSE;
    const char    *stage = "create";

    if (FAILED(StringCchPrintfW(path, MAX_PATH, L"%s\\bsnip-probe-%s.mp4",
                                dir, Encoder_Token(id))))
        return FALSE;
    DeleteFileW(path);

    if (FAILED(MFCreateAttributes(&attrs, 3)))
        return FALSE;
    IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                          &MFTranscodeContainerType_MPEG4);
    IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    hr = MFCreateSinkWriterFromURL(path, NULL, attrs, &writer);
    if (FAILED(hr)) goto done;

    stage = "configure";
    if (!Encoder_AddVideoStream(writer, id, PROBE_W, PROBE_H, PROBE_FPS, &stream)) {
        hr = E_FAIL;
        goto done;
    }

    stage = "begin";
    hr = IMFSinkWriter_BeginWriting(writer);
    if (FAILED(hr)) goto done;

    NameTransform(writer, stream, id);

    stage = "write";
    if (!PushProbeFrame(writer, stream, id)) { hr = E_FAIL; goto done; }

    stage = "finalise";
    hr = IMFSinkWriter_Finalize(writer);
    if (FAILED(hr)) goto done;
    ok = TRUE;

done:
    if (ok)
        Log_Printf(L"encoder: %s usable via %s%s", Encoder_Name(id),
                   g_caps.mft[id][0] ? g_caps.mft[id] : L"(unnamed)",
                   g_caps.hardware[id] ? L" [hardware]" : L"");
    else
        Log_Printf(L"encoder: %s unusable, failed at %hs (0x%08lX)",
                   Encoder_Name(id), stage, (unsigned long)hr);

    if (writer) IMFSinkWriter_Release(writer);
    if (attrs)  IMFAttributes_Release(attrs);
    DeleteFileW(path);
    return ok;
}

/* ------------------------------------------------------------- the cache */

/* Adapter plus user-mode driver version: a driver update can add or remove an
   encoder, and is the usual reason a cached answer goes stale. */
static void CacheKey(wchar_t *out, size_t cch)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;

    StringCchCopyW(out, cch, L"unknown");
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return;
    if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter))) {
        DXGI_ADAPTER_DESC1 ad;
        LARGE_INTEGER      umd;
        ZeroMemory(&umd, sizeof(umd));
        IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd);
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
            StringCchPrintfW(out, cch, L"%s %08lX%08lX", ad.Description,
                             (unsigned long)umd.HighPart,
                             (unsigned long)umd.LowPart);
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
}

#define CACHE_SEC L"EncoderCache"

static BOOL LoadCache(const wchar_t *key)
{
    wchar_t stored[256];
    EncoderId id;

    GetPrivateProfileStringW(CACHE_SEC, L"Key", L"", stored, ARRAYSIZE(stored),
                             Settings_IniPath());
    if (lstrcmpiW(stored, key) != 0)
        return FALSE;

    for (id = ENC_H264; id < ENC_COUNT; id++) {
        wchar_t name[32];
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s", Encoder_Token(id));
        g_caps.usable[id] = GetPrivateProfileIntW(CACHE_SEC, name, 0,
                                                  Settings_IniPath()) != 0;
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.mft", Encoder_Token(id));
        GetPrivateProfileStringW(CACHE_SEC, name, L"", g_caps.mft[id],
                                 ARRAYSIZE(g_caps.mft[id]), Settings_IniPath());
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.hw", Encoder_Token(id));
        g_caps.hardware[id] = GetPrivateProfileIntW(CACHE_SEC, name, 0,
                                                    Settings_IniPath()) != 0;
    }
    g_caps.probed = TRUE;
    return TRUE;
}

static void SaveCache(const wchar_t *key)
{
    EncoderId id;

    WritePrivateProfileStringW(CACHE_SEC, L"Key", key, Settings_IniPath());
    for (id = ENC_H264; id < ENC_COUNT; id++) {
        wchar_t name[32];
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s", Encoder_Token(id));
        WritePrivateProfileStringW(CACHE_SEC, name,
                                   g_caps.usable[id] ? L"1" : L"0",
                                   Settings_IniPath());
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.mft", Encoder_Token(id));
        WritePrivateProfileStringW(CACHE_SEC, name, g_caps.mft[id],
                                   Settings_IniPath());
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s.hw", Encoder_Token(id));
        WritePrivateProfileStringW(CACHE_SEC, name,
                                   g_caps.hardware[id] ? L"1" : L"0",
                                   Settings_IniPath());
    }
}

void Encoder_Probe(BOOL force)
{
    wchar_t key[256];
    wchar_t dir[MAX_PATH];
    EncoderId id;
    ULONGLONG t0;

    if (g_caps.probed && !force)
        return;

    AcquireSRWLockExclusive(&g_lock);
    if (g_caps.probed && !force) {       /* another thread got there first */
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    CacheKey(key, ARRAYSIZE(key));
    if (!force && LoadCache(key)) {
        Log_Printf(L"encoder: using cached probe for %s", key);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    if (!GetTempPathW(MAX_PATH, dir))
        StringCchCopyW(dir, MAX_PATH, L".");

    t0 = GetTickCount64();
    ZeroMemory(g_caps.usable, sizeof(g_caps.usable));
    ZeroMemory(g_caps.hardware, sizeof(g_caps.hardware));
    ZeroMemory(g_caps.mft, sizeof(g_caps.mft));

    for (id = ENC_H264; id < ENC_COUNT; id++)
        g_caps.usable[id] = ProbeOne(id, dir);

    g_caps.probed = TRUE;
    SaveCache(key);
    Log_Printf(L"encoder: probe took %lu ms on %s",
               (unsigned long)(GetTickCount64() - t0), key);
    ReleaseSRWLockExclusive(&g_lock);
}

static unsigned __stdcall ProbeThread(void *arg)
{
    UNREFERENCED_PARAMETER(arg);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    Encoder_Probe(FALSE);
    MFShutdown();
    CoUninitialize();
    InterlockedExchange(&g_probing, 0);
    return 0;
}

void Encoder_ProbeAsync(void)
{
    uintptr_t th;

    if (g_caps.probed)
        return;
    if (InterlockedCompareExchange(&g_probing, 1, 0) != 0)
        return;
    th = _beginthreadex(NULL, 0, ProbeThread, NULL, 0, NULL);
    if (!th)
        InterlockedExchange(&g_probing, 0);
    else
        CloseHandle((HANDLE)th);
}

/* ----------------------------------------------------------- the ranking */

/*
 * Best first, and "best" is not simply "newest". HEVC is the only way to carry
 * HDR here, so on an HDR display it wins outright. On an SDR one H.264 is
 * preferred despite being the older and larger codec: this machine has no
 * software HEVC encoder at all, Windows 10 needs the HEVC Video Extensions
 * package to play HEVC back in Photos or Films & TV, and a good many upload
 * targets reject it. The user can still pick HEVC explicitly.
 */
static const EncoderId g_rankHdr[] = { ENC_HEVC10, ENC_H264,  ENC_HEVC8 };
static const EncoderId g_rankSdr[] = { ENC_H264,   ENC_HEVC8, ENC_HEVC10 };

EncoderId Encoder_Resolve(EncoderId want, BOOL hdr)
{
    const EncoderId *rank = hdr ? g_rankHdr : g_rankSdr;
    const int        n = 3;
    int              i;

    if (!g_caps.probed)
        Encoder_Probe(FALSE);

    /* An explicit choice is honoured whenever it works. */
    if (want > ENC_AUTO && want < ENC_COUNT && g_caps.usable[want])
        return want;
    if (want > ENC_AUTO && want < ENC_COUNT)
        Log_Printf(L"encoder: %s was chosen but did not probe clean, ranking instead",
                   Encoder_Name(want));

    for (i = 0; i < n; i++)
        if (g_caps.usable[rank[i]])
            return rank[i];
    return ENC_COUNT;
}

EncoderId Encoder_Fallback(EncoderId id)
{
    /* Order by decreasing likelihood of working, ignoring quality: the point
       is to end up with a file rather than nothing. */
    static const EncoderId chain[] = { ENC_H264, ENC_HEVC8, ENC_HEVC10 };
    int i;

    for (i = 0; i < 3; i++)
        if (chain[i] != id && g_caps.usable[chain[i]])
            return chain[i];
    return ENC_COUNT;
}
