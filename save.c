/*
 * save.c - PNG encoding via WIC, the clipboard, and the file on disk.
 */

#define COBJMACROS

#include "nitshot.h"
#include "save.h"
#include "settings.h"
#include "hdr.h"
#include "log.h"

#include <wincodec.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <process.h>

static IWICImagingFactory *g_wic;
static UINT                g_cfPng;

BOOL Save_Init(void)
{
    if (!g_cfPng)
        g_cfPng = RegisterClipboardFormatW(L"PNG");

    if (g_wic)
        return TRUE;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&g_wic))) {
        g_wic = NULL;
        return FALSE;
    }
    return TRUE;
}

void Save_Shutdown(void)
{
    if (g_wic) {
        IWICImagingFactory_Release(g_wic);
        g_wic = NULL;
    }
}

/* ------------------------------------------------------------- encoding */

/*
 * Encode as PNG into a fresh heap block. NULL on failure.
 *
 * The PNG filter is pinned to Sub rather than left on WIC's adaptive default:
 * on an 8-megapixel screenshot the adaptive search dominated the whole snip,
 * and Sub costs only a few per cent of file size on screen content.
 */
static BYTE *EncodePng(const SnipImage *img, SIZE_T *sizeOut)
{
    IStream               *stream = NULL;
    IWICBitmapEncoder     *enc    = NULL;
    IWICBitmapFrameEncode *frame  = NULL;
    IPropertyBag2         *props  = NULL;
    WICPixelFormatGUID     fmt    = GUID_WICPixelFormat32bppBGRA;
    HGLOBAL                mem    = NULL;
    BYTE                  *out    = NULL;
    void                  *src;
    STATSTG                stat;

    *sizeOut = 0;
    if (!g_wic || !img || !img->pixels)
        return NULL;

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)))
        return NULL;
    if (FAILED(IWICImagingFactory_CreateEncoder(g_wic, &GUID_ContainerFormatPng,
                                                NULL, &enc)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props)))
        goto done;

    if (props) {
        PROPBAG2 opt;
        VARIANT  v;
        ZeroMemory(&opt, sizeof(opt));
        opt.pstrName = L"FilterOption";
        VariantInit(&v);
        v.vt   = VT_UI1;
        v.bVal = WICPngFilterSub;
        IPropertyBag2_Write(props, 1, &opt, &v);
    }

    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, props)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, img->width, img->height)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)))
        goto done;
    /* WIC negotiates: if it will not take our layout we would have to convert,
       and silently writing the wrong bytes is far worse than failing. */
    if (!IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, img->height, img->stride,
                                                 img->stride * img->height,
                                                 img->pixels)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Commit(enc)))
        goto done;

    if (FAILED(IStream_Stat(stream, &stat, STATFLAG_NONAME)))
        goto done;
    if (FAILED(GetHGlobalFromStream(stream, &mem)) || !mem)
        goto done;

    out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)stat.cbSize.QuadPart);
    if (!out)
        goto done;
    src = GlobalLock(mem);
    if (!src) {
        HeapFree(GetProcessHeap(), 0, out);
        out = NULL;
        goto done;
    }
    memcpy(out, src, (SIZE_T)stat.cbSize.QuadPart);
    GlobalUnlock(mem);
    *sizeOut = (SIZE_T)stat.cbSize.QuadPart;

done:
    if (props)  IPropertyBag2_Release(props);
    if (frame)  IWICBitmapFrameEncode_Release(frame);
    if (enc)    IWICBitmapEncoder_Release(enc);
    if (stream) IStream_Release(stream);
    return out;
}

/*
 * Encode an HDR image as JPEG XR: 64bpp RGBA half in scRGB, lossless.
 *
 * This is what Xbox Game Bar writes for its HDR captures and what the Photos
 * app reads back as HDR, and it is the only floating-point format the in-box
 * WIC codecs can write at all - PNG has no float, and AVIF/HEIF need codecs
 * from the Store that may not be installed.
 */
static BYTE *EncodeJxr(const SnipImage *img, SIZE_T *sizeOut)
{
    IStream               *stream = NULL;
    IWICBitmapEncoder     *enc    = NULL;
    IWICBitmapFrameEncode *frame  = NULL;
    IPropertyBag2         *props  = NULL;
    WICPixelFormatGUID     fmt    = GUID_WICPixelFormat64bppRGBAHalf;
    HGLOBAL                mem    = NULL;
    BYTE                  *out    = NULL;
    void                  *src;
    STATSTG                stat;

    *sizeOut = 0;
    if (!g_wic || !img || !img->pixels || !img->hdr)
        return NULL;

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)))
        return NULL;
    if (FAILED(IWICImagingFactory_CreateEncoder(g_wic, &GUID_ContainerFormatWmp,
                                                NULL, &enc)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props)))
        goto done;

    /*
     * Lossless is not a sensible default here. On a busy 3840x2160 desktop the
     * in-box encoder takes about 19 seconds for it and produces a 24 MB file;
     * quality 0.90 takes a little over two and produces 8 MB, and the
     * difference is not visible on screen content. Quality 100 selects true
     * lossless for anyone who wants it and is willing to wait.
     */
    if (props) {
        PROPBAG2 opt;
        VARIANT  v;
        ZeroMemory(&opt, sizeof(opt));
        VariantInit(&v);
        if (g_cfg.hdrJxrQuality >= 100) {
            opt.pstrName = L"Lossless";
            v.vt      = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
        } else {
            opt.pstrName = L"ImageQuality";
            v.vt     = VT_R4;
            v.fltVal = (float)g_cfg.hdrJxrQuality / 100.0f;
        }
        IPropertyBag2_Write(props, 1, &opt, &v);
    }

    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, props)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, img->width, img->height)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)))
        goto done;
    if (!IsEqualGUID(&fmt, &GUID_WICPixelFormat64bppRGBAHalf))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, img->height, img->stride,
                                                 img->stride * img->height,
                                                 img->pixels)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Commit(enc)))
        goto done;

    if (FAILED(IStream_Stat(stream, &stat, STATFLAG_NONAME)))
        goto done;
    if (FAILED(GetHGlobalFromStream(stream, &mem)) || !mem)
        goto done;

    out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)stat.cbSize.QuadPart);
    if (!out)
        goto done;
    src = GlobalLock(mem);
    if (!src) {
        HeapFree(GetProcessHeap(), 0, out);
        out = NULL;
        goto done;
    }
    memcpy(out, src, (SIZE_T)stat.cbSize.QuadPart);
    GlobalUnlock(mem);
    *sizeOut = (SIZE_T)stat.cbSize.QuadPart;

done:
    if (props)  IPropertyBag2_Release(props);
    if (frame)  IWICBitmapFrameEncode_Release(frame);
    if (enc)    IWICBitmapEncoder_Release(enc);
    if (stream) IStream_Release(stream);
    return out;
}

/*
 * The SDR view of an HDR capture: everything that is not a file on disk needs
 * it, because Windows has no HDR clipboard format and no HDR DIB.
 */
static BOOL ToneMapToSdr(const SnipImage *hdr, SnipImage *sdr)
{
    if (!hdr || !hdr->hdr || !Snip_AllocImage(sdr, hdr->width, hdr->height))
        return FALSE;
    Hdr_ToneMapScRgbToSrgb((const UINT16 *)hdr->pixels, hdr->stride / 2,
                           sdr->pixels, sdr->stride,
                           hdr->width, hdr->height, hdr->sdrWhiteNits,
                           g_cfg.hdrSdrRollOff);
    return TRUE;
}

/* ------------------------------------------------------------- the file */

/*
 * Picks a free "Screenshot YYYY-MM-DD HHMMSS.<ext>". An HDR snip and its
 * tone-mapped sidecar share the stem, so the two files sort together and it is
 * obvious they are the same capture; 'stem' returns it for the sidecar to use.
 */
static BOOL PickPath(wchar_t *path, size_t cch, const wchar_t *ext,
                     wchar_t *stem, size_t stemCch)
{
    wchar_t    dir[MAX_PATH];
    SYSTEMTIME st;
    int        n;

    if (!Settings_ResolveSaveFolder(dir, ARRAYSIZE(dir)))
        return FALSE;
    GetLocalTime(&st);

    for (n = 1; n <= 99; n++) {
        wchar_t base[MAX_PATH];
        HRESULT hr;

        if (n == 1)
            hr = StringCchPrintfW(base, ARRAYSIZE(base),
                    L"%s\\Screenshot %04u-%02u-%02u %02u%02u%02u",
                    dir, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
        else
            hr = StringCchPrintfW(base, ARRAYSIZE(base),
                    L"%s\\Screenshot %04u-%02u-%02u %02u%02u%02u (%d)",
                    dir, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, n);
        if (FAILED(hr))
            return FALSE;
        if (FAILED(StringCchPrintfW(path, cch, L"%s.%s", base, ext)))
            return FALSE;
        if (!PathFileExistsW(path)) {
            if (stem)
                StringCchCopyW(stem, stemCch, base);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL WriteWholeFile(const wchar_t *path, const BYTE *data, SIZE_T size)
{
    HANDLE f;
    DWORD  written = 0;
    BOOL   ok;

    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;

    ok = WriteFile(f, data, (DWORD)size, &written, NULL) && written == size;
    CloseHandle(f);

    /* A half-written file is worse than none: it looks like a saved snip. */
    if (!ok)
        DeleteFileW(path);
    return ok;
}

BOOL Save_ImageToPath(const SnipImage *img, const wchar_t *path)
{
    BYTE  *png;
    SIZE_T size = 0;
    BOOL   ok;

    if (!img || !img->pixels || !path || !Save_Init())
        return FALSE;
    png = EncodePng(img, &size);
    if (!png)
        return FALSE;
    ok = WriteWholeFile(path, png, size);
    HeapFree(GetProcessHeap(), 0, png);
    return ok;
}

/* -------------------------------------------------------- the clipboard */

/* A packed DIB (header + pixels), bottom-up, as CF_DIB and CF_DIBV5 want. */
static HGLOBAL MakeDib(const SnipImage *img, BOOL v5)
{
    SIZE_T   hdrSize = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    SIZE_T   pixels  = (SIZE_T)img->stride * img->height;
    HGLOBAL  mem;
    BYTE    *p;
    UINT32   y;

    mem = GlobalAlloc(GMEM_MOVEABLE, hdrSize + pixels);
    if (!mem)
        return NULL;
    p = (BYTE *)GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return NULL;
    }
    ZeroMemory(p, hdrSize);

    if (v5) {
        BITMAPV5HEADER *h = (BITMAPV5HEADER *)p;
        h->bV5Size        = sizeof(BITMAPV5HEADER);
        h->bV5Width       = (LONG)img->width;
        h->bV5Height      = (LONG)img->height;   /* positive: bottom-up */
        h->bV5Planes      = 1;
        h->bV5BitCount    = 32;
        h->bV5Compression = BI_BITFIELDS;
        h->bV5SizeImage   = (DWORD)pixels;
        h->bV5RedMask     = 0x00FF0000;
        h->bV5GreenMask   = 0x0000FF00;
        h->bV5BlueMask    = 0x000000FF;
        h->bV5AlphaMask   = 0xFF000000;
        h->bV5CSType      = LCS_sRGB;
        h->bV5Intent      = LCS_GM_IMAGES;
    } else {
        BITMAPINFOHEADER *h = (BITMAPINFOHEADER *)p;
        h->biSize        = sizeof(BITMAPINFOHEADER);
        h->biWidth       = (LONG)img->width;
        h->biHeight      = (LONG)img->height;
        h->biPlanes      = 1;
        h->biBitCount    = 32;
        h->biCompression = BI_RGB;
        h->biSizeImage   = (DWORD)pixels;
    }

    for (y = 0; y < img->height; y++) {
        const BYTE *src = img->pixels + (SIZE_T)(img->height - 1 - y) * img->stride;
        BYTE       *dst = p + hdrSize + (SIZE_T)y * img->stride;

        memcpy(dst, src, img->stride);

        /*
         * CF_DIB has no alpha, so a freeform cut-out would paste as a shape on
         * a black background in anything that does not understand CF_DIBV5 or
         * PNG. The pixels are premultiplied, so compositing them over white is
         * just adding back the part the shape does not cover.
         */
        if (!v5) {
            UINT32 x;
            for (x = 0; x < img->width; x++) {
                unsigned a = dst[x * 4 + 3];
                if (a != 255) {
                    unsigned inv = 255 - a;
                    dst[x * 4 + 0] = (BYTE)(dst[x * 4 + 0] + inv);
                    dst[x * 4 + 1] = (BYTE)(dst[x * 4 + 1] + inv);
                    dst[x * 4 + 2] = (BYTE)(dst[x * 4 + 2] + inv);
                }
                dst[x * 4 + 3] = 0xFF;
            }
        }
    }
    GlobalUnlock(mem);
    return mem;
}

static HGLOBAL CopyToGlobal(const BYTE *data, SIZE_T size)
{
    HGLOBAL mem;
    void   *p;

    if (!data || size == 0)
        return NULL;
    mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem)
        return NULL;
    p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return NULL;
    }
    memcpy(p, data, size);
    GlobalUnlock(mem);
    return mem;
}

static BOOL PutOnClipboard(HWND owner, const SnipImage *img,
                           const BYTE *png, SIZE_T pngSize)
{
    HGLOBAL dib, dibv5, pngMem;
    BOOL    any = FALSE;

    dib    = MakeDib(img, FALSE);
    dibv5  = MakeDib(img, TRUE);
    pngMem = CopyToGlobal(png, pngSize);
    if (!dib && !dibv5 && !pngMem)
        return FALSE;

    if (!OpenClipboard(owner)) {
        /* Another process can hold the clipboard for a moment; one retry is
           enough in practice and keeps the capture from being lost. */
        Sleep(60);
        if (!OpenClipboard(owner)) {
            if (dib)    GlobalFree(dib);
            if (dibv5)  GlobalFree(dibv5);
            if (pngMem) GlobalFree(pngMem);
            return FALSE;
        }
    }
    EmptyClipboard();

    /* Most specific first - some apps take the first format they recognise. */
    if (pngMem && SetClipboardData(g_cfPng, pngMem)) { any = TRUE; pngMem = NULL; }
    if (dibv5  && SetClipboardData(CF_DIBV5, dibv5)) { any = TRUE; dibv5  = NULL; }
    if (dib    && SetClipboardData(CF_DIB,   dib))   { any = TRUE; dib    = NULL; }

    CloseClipboard();

    /* Whatever the clipboard did not take is still ours to free. */
    if (dib)    GlobalFree(dib);
    if (dibv5)  GlobalFree(dibv5);
    if (pngMem) GlobalFree(pngMem);
    return any;
}

/* ------------------------------------------------------------ the entry */

BOOL Save_Snip(HWND owner, const SnipImage *img, SaveResult *res)
{
    SnipImage        sdr;
    const SnipImage *flat = img;     /* the 8-bit view of whatever came in */
    BYTE            *png  = NULL;
    BYTE            *jxr  = NULL;
    SIZE_T           pngSize = 0, jxrSize = 0;
    wchar_t          stem[MAX_PATH];
    DWORD            t0;

    if (!res)
        return FALSE;
    ZeroMemory(res, sizeof(*res));
    ZeroMemory(&sdr, sizeof(sdr));
    stem[0] = 0;
    if (!img || !img->pixels)
        return FALSE;
    if (!Save_Init())
        return FALSE;

    t0 = GetTickCount();

    /*
     * An HDR snip produces up to two files. The .jxr is the capture itself,
     * with the highlights intact. Everything else - the clipboard, the DIB
     * formats, the optional sidecar - has to be the tone-mapped 8-bit view,
     * because Windows has no HDR clipboard format and no HDR DIB.
     */
    if (img->hdr) {
        DWORD t1, t2;

        t1 = GetTickCount();
        if (g_cfg.hdrSaveJxr && g_cfg.saveToDisk) {
            jxr = EncodeJxr(img, &jxrSize);
            if (!jxr)
                Log_Printf(L"save: JPEG XR encode failed, falling back to PNG");
        }
        t2 = GetTickCount();
        if (ToneMapToSdr(img, &sdr))
            flat = &sdr;
        else
            flat = NULL;             /* nothing 8-bit can be produced */
        Log_Printf(L"save: jxr %lu ms, tonemap %lu ms",
                   (unsigned long)(t2 - t1),
                   (unsigned long)(GetTickCount() - t2));
    }

    /* One PNG encode feeds the sidecar, the SDR file and the clipboard. */
    if (flat && (g_cfg.saveToDisk || g_cfg.copyToClipboard)) {
        DWORD t1 = GetTickCount();
        png = EncodePng(flat, &pngSize);
        Log_Printf(L"save: png %lu ms", (unsigned long)(GetTickCount() - t1));
    }

    res->encodeMs = (long)(GetTickCount() - t0);
    res->hdr      = img->hdr;

    if (g_cfg.saveToDisk) {
        if (jxr) {
            if (PickPath(res->path, ARRAYSIZE(res->path), L"jxr",
                         stem, ARRAYSIZE(stem)))
                res->diskOk = WriteWholeFile(res->path, jxr, jxrSize);

            /* The sidecar shares the stem so the pair sorts together. */
            if (res->diskOk && png && g_cfg.hdrSdrSidecar) {
                wchar_t side[MAX_PATH];
                if (SUCCEEDED(StringCchPrintfW(side, ARRAYSIZE(side), L"%s.png", stem)))
                    res->sidecarOk = WriteWholeFile(side, png, pngSize);
            }
        } else if (png) {
            if (PickPath(res->path, ARRAYSIZE(res->path), L"png", NULL, 0))
                res->diskOk = WriteWholeFile(res->path, png, pngSize);
        }
        if (!res->diskOk)
            res->path[0] = 0;
    }

    /* The DIB formats do not need the PNG, so a failed encode still gets the
       image onto the clipboard. */
    if (g_cfg.copyToClipboard && flat)
        res->clipboardOk = PutOnClipboard(owner, flat, png, pngSize);

    if (png)
        HeapFree(GetProcessHeap(), 0, png);
    if (jxr)
        HeapFree(GetProcessHeap(), 0, jxr);
    Snip_FreeImage(&sdr);
    return res->diskOk || res->clipboardOk;
}

/* ------------------------------------------------------- the worker */

typedef struct {
    HWND      notify;
    UINT      doneMsg;
    SnipImage img;        /* owned by the worker */
} SaveJob;

void Save_FreeResult(SaveResult *res)
{
    if (res)
        HeapFree(GetProcessHeap(), 0, res);
}

static unsigned __stdcall SaveThread(void *arg)
{
    SaveJob    *job = (SaveJob *)arg;
    SaveResult *res;

    /* WIC is COM, and this thread is not the one that initialised it. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    res = (SaveResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*res));
    if (res)
        Save_Snip(job->notify, &job->img, res);

    Snip_FreeImage(&job->img);
    CoUninitialize();

    if (res && !PostMessageW(job->notify, job->doneMsg, 0, (LPARAM)res))
        Save_FreeResult(res);          /* the window went away first */

    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

BOOL Save_SnipAsync(HWND notify, UINT doneMsg, SnipImage *img)
{
    SaveJob  *job;
    uintptr_t th;

    if (!img || !img->pixels)
        return FALSE;

    job = (SaveJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*job));
    if (!job) {
        Snip_FreeImage(img);
        return FALSE;
    }
    job->notify  = notify;
    job->doneMsg = doneMsg;
    job->img     = *img;
    ZeroMemory(img, sizeof(*img));     /* ownership has moved */

    th = _beginthreadex(NULL, 0, SaveThread, job, 0, NULL);
    if (!th) {
        Snip_FreeImage(&job->img);
        HeapFree(GetProcessHeap(), 0, job);
        return FALSE;
    }
    CloseHandle((HANDLE)th);
    return TRUE;
}
