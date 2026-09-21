/*
 * audio.c - WASAPI shared-mode capture, loopback or microphone.
 */

#define COBJMACROS

#include "nitshot.h"
#include "audio.h"
#include "settings.h"
#include "log.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <process.h>

/*
 * The WASAPI headers only declare these; the definitions live in a generated
 * object that no import library on this SDK carries, and the class id is a C++
 * class declaration that C cannot see at all. Every value is taken from the
 * MIDL_INTERFACE attributes in mmdeviceapi.h / Audioclient.h and ksmedia.h.
 */
static const CLSID CLSID_MMDeviceEnumerator_ =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID IID_IMMDeviceEnumerator_ =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID IID_IAudioClient_ =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID IID_IAudioCaptureClient_ =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const IID IID_IAudioClient2_ =
    { 0x726778CD, 0xF60A, 0x4EDA, { 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA } };
static const GUID SUBTYPE_IEEE_FLOAT_ =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

#define BUFFER_DURATION_100NS  2000000   /* 200 ms of endpoint buffer */

static IAudioClient        *g_client;
static IAudioCaptureClient *g_capture;
static WAVEFORMATEX        *g_mix;
static HANDLE               g_thread;
static HANDLE               g_stopEvent;
static AudioSinkFn          g_sink;
static void                *g_ctx;
static UINT32               g_outChannels;

/* ------------------------------------------------------------ conversion */

/*
 * The mixer hands over whatever the endpoint runs at - practically always
 * 32-bit float - and the AAC encoder wants 16-bit PCM. Channels beyond the
 * first two are dropped rather than properly downmixed: a correct 5.1 fold-down
 * needs the channel mask and matrix coefficients, and for a screen recording
 * the front pair is what matters.
 */
static BOOL IsFloat(const WAVEFORMATEX *f)
{
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return TRUE;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
        return IsEqualGUID(&e->SubFormat, &SUBTYPE_IEEE_FLOAT_);
    }
    return FALSE;
}

static INT16 ClampToPcm(float v)
{
    float s = v * 32767.0f;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (INT16)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

static void Convert(const BYTE *src, UINT32 frames, INT16 *dst)
{
    UINT32 i, c;
    UINT32 inCh  = g_mix->nChannels;
    UINT32 outCh = g_outChannels;

    if (IsFloat(g_mix)) {
        const float *f = (const float *)src;
        for (i = 0; i < frames; i++) {
            for (c = 0; c < outCh; c++)
                dst[i * outCh + c] = ClampToPcm(f[i * inCh + c]);
        }
    } else if (g_mix->wBitsPerSample == 16) {
        const INT16 *s = (const INT16 *)src;
        for (i = 0; i < frames; i++) {
            for (c = 0; c < outCh; c++)
                dst[i * outCh + c] = s[i * inCh + c];
        }
    } else {
        ZeroMemory(dst, (SIZE_T)frames * outCh * sizeof(INT16));
    }
}

/* ---------------------------------------------------------------- thread */

static unsigned __stdcall CaptureThread(void *arg)
{
    INT16 *scratch = NULL;
    UINT32 scratchFrames = 0;
    /*
     * Poll far more often than the endpoint buffer is long. Half the buffer
     * looks like it ought to be enough and is not: the thread only has to be
     * descheduled once for the buffer to overrun, and WASAPI's answer to an
     * overrun is to throw the samples away. It also decides how lumpy the
     * delivery is, and lumpy delivery upsets the recorder's timeline.
     */
    DWORD  waitMs = 10;

    UNREFERENCED_PARAMETER(arg);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (WaitForSingleObject(g_stopEvent, waitMs) == WAIT_TIMEOUT) {
        UINT32 packet = 0;

        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(g_capture, &packet)) &&
               packet > 0) {
            BYTE   *data  = NULL;
            UINT32  frames = 0;
            DWORD   flags = 0;

            if (FAILED(IAudioCaptureClient_GetBuffer(g_capture, &data, &frames,
                                                     &flags, NULL, NULL)))
                break;

            if (frames > scratchFrames) {
                INT16 *n = (INT16 *)(scratch
                    ? HeapReAlloc(GetProcessHeap(), 0, scratch,
                                  (SIZE_T)frames * g_outChannels * sizeof(INT16))
                    : HeapAlloc(GetProcessHeap(), 0,
                                (SIZE_T)frames * g_outChannels * sizeof(INT16)));
                if (!n) {
                    IAudioCaptureClient_ReleaseBuffer(g_capture, frames);
                    break;
                }
                scratch = n;
                scratchFrames = frames;
            }

            /* A loopback stream reports silence rather than delivering zeros,
               and the encoder still needs those samples to keep its timeline. */
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                ZeroMemory(scratch, (SIZE_T)frames * g_outChannels * sizeof(INT16));
            else
                Convert(data, frames, scratch);

            if (g_sink)
                g_sink(g_ctx, (const BYTE *)scratch, frames);

            IAudioCaptureClient_ReleaseBuffer(g_capture, frames);
        }
    }

    if (scratch)
        HeapFree(GetProcessHeap(), 0, scratch);
    CoUninitialize();
    return 0;
}

/* ----------------------------------------------------------------- setup */

static void Cleanup(void)
{
    if (g_capture) { IAudioCaptureClient_Release(g_capture); g_capture = NULL; }
    if (g_client)  { IAudioClient_Stop(g_client);
                     IAudioClient_Release(g_client);         g_client  = NULL; }
    if (g_mix)     { CoTaskMemFree(g_mix);                   g_mix     = NULL; }
    if (g_stopEvent) { CloseHandle(g_stopEvent);             g_stopEvent = NULL; }
    if (g_thread)  { CloseHandle(g_thread);                  g_thread  = NULL; }
    g_sink = NULL;
    g_ctx  = NULL;
}

BOOL Audio_Start(AudioSource src, AudioFormat *fmt, AudioSinkFn sink, void *ctx)
{
    IMMDeviceEnumerator *devEnum = NULL;
    IMMDevice           *device  = NULL;
    uintptr_t            th;
    DWORD                streamFlags = 0;
    BOOL                 ok = FALSE;

    if (src == AUDIO_NONE || !fmt || !sink)
        return FALSE;
    Audio_Stop();

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_, (void **)&devEnum)))
        return FALSE;

    /* Loopback captures the render endpoint, which is why system audio asks
       for eRender rather than eCapture. */
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devEnum,
            src == AUDIO_SYSTEM ? eRender : eCapture, eConsole, &device)))
        goto done;

    if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient_, CLSCTX_ALL, NULL,
                                  (void **)&g_client)))
        goto done;
    /*
     * Ask for the raw stream, which bypasses the endpoint's effects.
     *
     * This matters more than it sounds. A loopback capture taps what the audio
     * engine is playing, and on a device with enhancements switched on that is
     * the *processed* signal - measured here, a bass and treble lift of up to
     * 18 dB against the middle. Recording that and then playing the recording
     * back through the same enhancements applies the curve twice, which is
     * exactly the muddy, damped sound the whole thing was reported for.
     *
     * Not every driver honours it - the EPOS GSX 300 here answers
     * AUDCLNT_E_RAW_MODE_UNSUPPORTED (0x88890027) and keeps its effects in the
     * chain - so a refusal is logged and otherwise ignored. On such a device
     * the only way to record the untouched signal is to turn the endpoint's
     * enhancements off in Sound settings.
     */
    if (!g_cfg.recordRawAudio) {
        Log_Printf(L"audio: raw mode disabled by settings");
    } else {
        IAudioClient2 *client2 = NULL;
        if (SUCCEEDED(IAudioClient_QueryInterface(g_client, &IID_IAudioClient2_,
                                                  (void **)&client2))) {
            AudioClientProperties props;
            HRESULT hr;
            ZeroMemory(&props, sizeof(props));
            props.cbSize     = sizeof(props);
            props.bIsOffload = FALSE;
            props.eCategory  = AudioCategory_Other;
            props.Options    = AUDCLNT_STREAMOPTIONS_RAW;
            hr = IAudioClient2_SetClientProperties(client2, &props);
            if (SUCCEEDED(hr))
                Log_Printf(L"audio: raw mode on, endpoint effects bypassed");
            else
                Log_Printf(L"audio: raw mode refused (0x%08lX); the endpoint's "
                           L"effects stay in the captured signal",
                           (unsigned long)hr);
            IAudioClient2_Release(client2);
        }
    }

    if (FAILED(IAudioClient_GetMixFormat(g_client, &g_mix)))
        goto done;

    /* The AAC encoder takes 44.1 or 48 kHz only. Resampling here would be a
       module of its own, so an exotic endpoint is reported rather than faked. */
    if (g_mix->nSamplesPerSec != 44100 && g_mix->nSamplesPerSec != 48000) {
        Log_Printf(L"audio: endpoint runs at %lu Hz, which AAC cannot take",
                   (unsigned long)g_mix->nSamplesPerSec);
        goto done;
    }

    g_outChannels = g_mix->nChannels >= 2 ? 2 : 1;

    if (src == AUDIO_SYSTEM)
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;

    if (FAILED(IAudioClient_Initialize(g_client, AUDCLNT_SHAREMODE_SHARED,
                                       streamFlags, BUFFER_DURATION_100NS, 0,
                                       g_mix, NULL)))
        goto done;
    if (FAILED(IAudioClient_GetService(g_client, &IID_IAudioCaptureClient_,
                                       (void **)&g_capture)))
        goto done;

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent)
        goto done;

    g_sink = sink;
    g_ctx  = ctx;

    if (FAILED(IAudioClient_Start(g_client)))
        goto done;

    th = _beginthreadex(NULL, 0, CaptureThread, NULL, 0, NULL);
    if (!th)
        goto done;
    g_thread = (HANDLE)th;

    fmt->sampleRate = g_mix->nSamplesPerSec;
    fmt->channels   = g_outChannels;
    ok = TRUE;
    Log_Printf(L"audio: %s at %lu Hz, %u channel(s)",
               src == AUDIO_SYSTEM ? L"system loopback" : L"microphone",
               (unsigned long)fmt->sampleRate, fmt->channels);

done:
    if (device)  IMMDevice_Release(device);
    if (devEnum) IMMDeviceEnumerator_Release(devEnum);
    if (!ok)
        Cleanup();
    return ok;
}

void Audio_Stop(void)
{
    if (g_stopEvent)
        SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    Cleanup();
}
