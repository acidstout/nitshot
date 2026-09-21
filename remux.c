/*
 * remux.c - copy the video through, encode the WAV, write a new MP4.
 */

#define COBJMACROS

#include "nitshot.h"
#include "remux.h"
#include "log.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <strsafe.h>

/* The AAC encoder offers 96, 128, 160 and 192 kbps and nothing else; this is
   the top of that list, expressed the way the media type wants it. */
#define AAC_BYTES_PER_SECOND  24000

/* The MF_SOURCE_READER_* selectors are signed constants with the top bit set,
   and every function takes them as DWORD. */
#define SEL_ALL    ((DWORD)MF_SOURCE_READER_ALL_STREAMS)
#define SEL_VIDEO  ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)
#define SEL_AUDIO  ((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM)

static BOOL OpenVideoReader(const wchar_t *path, IMFSourceReader **reader,
                            IMFMediaType **type)
{
    IMFAttributes *attrs = NULL;
    HRESULT        hr;

    *reader = NULL;
    *type   = NULL;

    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return FALSE;
    /* Deliberately no decoding and no video processing: leaving the stream
       alone is the entire point, and on this machine there is no HEVC decoder
       to invoke even if we wanted one. */
    hr = MFCreateSourceReaderFromURL(path, attrs, reader);
    IMFAttributes_Release(attrs);
    if (FAILED(hr)) {
        Log_Printf(L"remux: cannot read %s (0x%08lX)", path, (unsigned long)hr);
        return FALSE;
    }

    IMFSourceReader_SetStreamSelection(*reader, SEL_ALL, FALSE);
    IMFSourceReader_SetStreamSelection(*reader, SEL_VIDEO, TRUE);

    if (FAILED(IMFSourceReader_GetCurrentMediaType(*reader,
            SEL_VIDEO, type))) {
        Log_Printf(L"remux: the source has no video stream");
        IMFSourceReader_Release(*reader);
        *reader = NULL;
        return FALSE;
    }
    return TRUE;
}

static BOOL OpenWavReader(const wchar_t *path, IMFSourceReader **reader,
                          IMFMediaType **type)
{
    IMFMediaType *want = NULL;
    HRESULT       hr;

    *reader = NULL;
    *type   = NULL;

    hr = MFCreateSourceReaderFromURL(path, NULL, reader);
    if (FAILED(hr)) {
        Log_Printf(L"remux: cannot read %s (0x%08lX)", path, (unsigned long)hr);
        return FALSE;
    }
    IMFSourceReader_SetStreamSelection(*reader, SEL_ALL, FALSE);
    IMFSourceReader_SetStreamSelection(*reader, SEL_AUDIO, TRUE);

    /* Ask for 16-bit PCM explicitly. The sidecar already is that, so this is a
       no-op conversion, but stating it means the input type below is certain. */
    if (SUCCEEDED(MFCreateMediaType(&want))) {
        IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(want, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFSourceReader_SetCurrentMediaType(*reader,
            SEL_AUDIO, NULL, want);
        IMFMediaType_Release(want);
    }

    if (FAILED(IMFSourceReader_GetCurrentMediaType(*reader,
            SEL_AUDIO, type))) {
        Log_Printf(L"remux: the sidecar has no readable audio");
        IMFSourceReader_Release(*reader);
        *reader = NULL;
        return FALSE;
    }
    return TRUE;
}

/*
 * A fresh type carrying only what the sink needs, rather than the reader's own.
 *
 * Two things go wrong when the reader's type is handed straight back. Its
 * MF_MT_FRAME_SIZE is the *coded* size - HEVC pads the picture up to whole
 * coding units, so a 720-line recording reports 736 - and the sink writes that
 * into tkhd, which makes players show sixteen rows of padding. The real size is
 * in MF_MT_MINIMUM_DISPLAY_APERTURE.
 *
 * Copying the attributes wholesale also makes the sink write a degenerate
 * sample description for HEVC: the 'hvc1' entry ends up directly inside 'stbl'
 * with no 'stsd' box around it, and the resulting file has a video track that
 * Media Foundation itself can no longer find. Building the type from scratch
 * avoids it.
 */
static BOOL MakeVideoOutType(IMFMediaType *src, IMFMediaType **out)
{
    MFVideoArea area;
    UINT64      packed = 0;
    UINT32      got = 0, u32 = 0;
    UINT8      *blob = NULL;
    GUID        subtype;

    *out = NULL;
    if (FAILED(MFCreateMediaType(out)))
        return FALSE;

    IMFMediaType_SetGUID(*out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(IMFMediaType_GetGUID(src, &MF_MT_SUBTYPE, &subtype)))
        IMFMediaType_SetGUID(*out, &MF_MT_SUBTYPE, &subtype);
    IMFMediaType_SetUINT32(*out, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);

    /* The visible picture, not the padded coding grid. */
    if (SUCCEEDED(IMFMediaType_GetBlob(src, &MF_MT_MINIMUM_DISPLAY_APERTURE,
                                       (UINT8 *)&area, sizeof(area), &got)) &&
        got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
        IMFMediaType_SetUINT64(*out, &MF_MT_FRAME_SIZE,
                               ((UINT64)(UINT32)area.Area.cx << 32) |
                               (UINT32)area.Area.cy);
    } else if (SUCCEEDED(IMFMediaType_GetUINT64(src, &MF_MT_FRAME_SIZE, &packed))) {
        IMFMediaType_SetUINT64(*out, &MF_MT_FRAME_SIZE, packed);
    }

    if (SUCCEEDED(IMFMediaType_GetUINT64(src, &MF_MT_FRAME_RATE, &packed)))
        IMFMediaType_SetUINT64(*out, &MF_MT_FRAME_RATE, packed);
    if (SUCCEEDED(IMFMediaType_GetUINT64(src, &MF_MT_PIXEL_ASPECT_RATIO, &packed)))
        IMFMediaType_SetUINT64(*out, &MF_MT_PIXEL_ASPECT_RATIO, packed);
    else
        IMFMediaType_SetUINT64(*out, &MF_MT_PIXEL_ASPECT_RATIO,
                               ((UINT64)1 << 32) | 1);
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_AVG_BITRATE, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_AVG_BITRATE, u32);

    /* The colour description, so an HDR recording stays an HDR recording. */
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_VIDEO_PRIMARIES, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_VIDEO_PRIMARIES, u32);
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_TRANSFER_FUNCTION, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_TRANSFER_FUNCTION, u32);
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_YUV_MATRIX, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_YUV_MATRIX, u32);
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_VIDEO_NOMINAL_RANGE, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_VIDEO_NOMINAL_RANGE, u32);
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_MPEG2_PROFILE, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_MPEG2_PROFILE, u32);
    if (SUCCEEDED(IMFMediaType_GetUINT32(src, &MF_MT_MPEG2_LEVEL, &u32)))
        IMFMediaType_SetUINT32(*out, &MF_MT_MPEG2_LEVEL, u32);

    /*
     * The codec private data - what becomes hvcC or avcC in the new file.
     *
     * An MP4 source does not offer MF_MT_MPEG_SEQUENCE_HEADER; it offers
     * MF_MT_MPEG4_SAMPLE_DESCRIPTION, which is the whole sample entry box.
     * Handing *that* back is precisely what makes the sink drop the entry into
     * 'stbl' unwrapped, so the configuration record is dug out of it and
     * passed as a sequence header instead, which is the form the sink builds a
     * correct sample description from.
     */
    if (SUCCEEDED(IMFMediaType_GetAllocatedBlob(src, &MF_MT_MPEG_SEQUENCE_HEADER,
                                                &blob, &got))) {
        IMFMediaType_SetBlob(*out, &MF_MT_MPEG_SEQUENCE_HEADER, blob, got);
        CoTaskMemFree(blob);
        return TRUE;
    }

    if (SUCCEEDED(IMFMediaType_GetAllocatedBlob(src,
            &MF_MT_MPEG4_SAMPLE_DESCRIPTION, &blob, &got))) {
        /*
         * The two sides disagree about what this blob is. The source reader
         * hands back the sample *entry* - the 'hvc1' box on its own - while
         * the sink writes whatever it is given straight into the place the
         * 'stsd' box belongs. Give it back unaltered and the file ends up with
         * 'hvc1' sitting directly inside 'stbl', which leaves a video track
         * that Media Foundation itself can no longer find. Wrapping the entry
         * in the stsd box the sink is expecting is the whole fix.
         */
        UINT8 *wrapped = (UINT8 *)HeapAlloc(GetProcessHeap(), 0,
                                            (SIZE_T)got + 16);
        if (wrapped) {
            UINT32 total = got + 16;
            wrapped[0] = (UINT8)(total >> 24); wrapped[1] = (UINT8)(total >> 16);
            wrapped[2] = (UINT8)(total >> 8);  wrapped[3] = (UINT8)total;
            wrapped[4] = 's'; wrapped[5] = 't'; wrapped[6] = 's'; wrapped[7] = 'd';
            wrapped[8] = wrapped[9] = wrapped[10] = wrapped[11] = 0; /* version, flags */
            wrapped[12] = wrapped[13] = wrapped[14] = 0;
            wrapped[15] = 1;                                         /* entry count */
            memcpy(wrapped + 16, blob, got);

            IMFMediaType_SetBlob(*out, &MF_MT_MPEG4_SAMPLE_DESCRIPTION,
                                 wrapped, total);
            HeapFree(GetProcessHeap(), 0, wrapped);
            CoTaskMemFree(blob);
            return TRUE;
        }
        CoTaskMemFree(blob);
    }

    Log_Printf(L"remux: the source carries no codec private data");
    IMFMediaType_Release(*out);
    *out = NULL;
    return FALSE;
}

static BOOL AddAacStream(IMFSinkWriter *writer, IMFMediaType *pcm, DWORD *index)
{
    IMFMediaType *out = NULL;
    UINT32        rate = 0, channels = 0;
    BOOL          ok = FALSE;

    IMFMediaType_GetUINT32(pcm, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(pcm, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (!rate || !channels) {
        Log_Printf(L"remux: the sidecar's format could not be read");
        return FALSE;
    }

    if (FAILED(MFCreateMediaType(&out)))
        return FALSE;
    IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_NUM_CHANNELS, channels);
    IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           AAC_BYTES_PER_SECOND);

    if (SUCCEEDED(IMFSinkWriter_AddStream(writer, out, index)) &&
        SUCCEEDED(IMFSinkWriter_SetInputMediaType(writer, *index, pcm, NULL)))
        ok = TRUE;

    IMFMediaType_Release(out);
    return ok;
}

/*
 * Reads one sample and writes it on. Returns FALSE when that stream is spent.
 * 'when' receives the sample's timestamp so the caller can keep the two
 * streams roughly level and stop the sink buffering a whole recording.
 */
static BOOL PumpOne(IMFSourceReader *reader, DWORD srcStream,
                    IMFSinkWriter *writer, DWORD dstStream, LONGLONG *when)
{
    IMFSample *sample = NULL;
    DWORD      actual = 0, flags = 0;
    LONGLONG   ts = 0;
    BOOL       more = TRUE;

    if (FAILED(IMFSourceReader_ReadSample(reader, srcStream, 0, &actual, &flags,
                                          &ts, &sample)))
        return FALSE;

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        more = FALSE;

    if (sample) {
        IMFSinkWriter_WriteSample(writer, dstStream, sample);
        IMFSample_Release(sample);
        *when = ts;
    } else if (more) {
        /* A stream tick or a format change with no data; nothing to pass on,
           but the stream has not ended. */
        *when = ts;
    }
    return more;
}

BOOL Remux_AddWavAudio(const wchar_t *mp4, const wchar_t *wav)
{
    IMFSourceReader *vid = NULL, *aud = NULL;
    IMFMediaType    *vidType = NULL, *pcmType = NULL, *outVidType = NULL;
    IMFSinkWriter   *writer = NULL;
    IMFAttributes   *attrs = NULL;
    wchar_t          temp[MAX_PATH];
    DWORD            vStream = 0, aStream = 0;
    LONGLONG         vTime = 0, aTime = 0;
    BOOL             vMore = TRUE, aMore = TRUE;
    BOOL             ok = FALSE;
    ULONGLONG        t0 = GetTickCount64();
    HRESULT          hr;

    if (!mp4 || !wav || !PathFileExistsW(mp4) || !PathFileExistsW(wav))
        return FALSE;

    if (FAILED(StringCchPrintfW(temp, MAX_PATH, L"%s.remux.tmp", mp4)))
        return FALSE;
    DeleteFileW(temp);

    if (!OpenVideoReader(mp4, &vid, &vidType)) goto done;
    if (!OpenWavReader(wav, &aud, &pcmType))   goto done;

    if (FAILED(MFCreateAttributes(&attrs, 3)))
        goto done;
    IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE,
                          &MFTranscodeContainerType_MPEG4);
    IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    hr = MFCreateSinkWriterFromURL(temp, NULL, attrs, &writer);
    if (FAILED(hr)) {
        Log_Printf(L"remux: cannot create %s (0x%08lX)", temp, (unsigned long)hr);
        goto done;
    }

    /*
     * Output and input are the same type, which is what makes the sink pass
     * the samples through untouched instead of looking for an encoder.
     */
    if (!MakeVideoOutType(vidType, &outVidType))
        goto done;
    if (FAILED(IMFSinkWriter_AddStream(writer, outVidType, &vStream)) ||
        FAILED(IMFSinkWriter_SetInputMediaType(writer, vStream, outVidType, NULL))) {
        Log_Printf(L"remux: the video stream could not be copied through");
        goto done;
    }
    if (!AddAacStream(writer, pcmType, &aStream))
        goto done;

    if (FAILED(IMFSinkWriter_BeginWriting(writer))) {
        Log_Printf(L"remux: the writer refused to start");
        goto done;
    }

    /* Feed whichever stream is behind, so the sink never has to hold a whole
       recording of one of them while waiting for the other. */
    while (vMore || aMore) {
        if (vMore && (!aMore || vTime <= aTime))
            vMore = PumpOne(vid, SEL_VIDEO,
                            writer, vStream, &vTime);
        else if (aMore)
            aMore = PumpOne(aud, SEL_AUDIO,
                            writer, aStream, &aTime);
    }

    hr = IMFSinkWriter_Finalize(writer);
    if (FAILED(hr)) {
        Log_Printf(L"remux: finalize failed 0x%08lX", (unsigned long)hr);
        goto done;
    }

    /* Release before moving; the sink keeps the file open until it is gone. */
    IMFSinkWriter_Release(writer); writer = NULL;
    IMFSourceReader_Release(vid);  vid    = NULL;
    IMFSourceReader_Release(aud);  aud    = NULL;

    if (!MoveFileExW(temp, mp4, MOVEFILE_REPLACE_EXISTING)) {
        Log_Printf(L"remux: could not replace %s (%lu)", mp4, GetLastError());
        goto done;
    }
    ok = TRUE;
    Log_Printf(L"remux: sound built from the sidecar in %lu ms",
               (unsigned long)(GetTickCount64() - t0));

done:
    if (writer)  IMFSinkWriter_Release(writer);
    if (vid)     IMFSourceReader_Release(vid);
    if (aud)     IMFSourceReader_Release(aud);
    if (outVidType) IMFMediaType_Release(outVidType);
    if (vidType) IMFMediaType_Release(vidType);
    if (pcmType) IMFMediaType_Release(pcmType);
    if (attrs)   IMFAttributes_Release(attrs);
    if (!ok)
        DeleteFileW(temp);     /* the original is still intact */
    return ok;
}
