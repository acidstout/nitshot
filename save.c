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

/* Used only on the UI thread, by the overlay's debug dump. Every save worker
   makes its own: a COM object belongs to the apartment that created it, and
   handing this one to other threads broke that rule. */
static IWICImagingFactory *g_wic;
static UINT                g_cfPng;

/*
 * How many Save_SnipAsync threads are still running, and an event that is set
 * whenever the count reaches zero. Encoding a 4K JPEG XR takes seconds -
 * lossless takes about twenty - and quitting in the middle of that would
 * leave a half-written file behind, so shutdown waits for them.
 */
static volatile LONG g_pending;
static HANDLE        g_idle;

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

/* Lets the next waiter through once the last encode has finished. */
static void SaveDone(void)
{
    if (InterlockedDecrement(&g_pending) == 0 && g_idle)
        SetEvent(g_idle);
}

BOOL Save_WaitForPending(DWORD timeoutMs)
{
    LONG n = InterlockedCompareExchange(&g_pending, 0, 0);

    if (n == 0 || !g_idle)
        return TRUE;
    Log_Printf(L"save: waiting for %ld encode(s) to finish", n);
    return WaitForSingleObject(g_idle, timeoutMs) == WAIT_OBJECT_0;
}

void Save_Shutdown(void)
{
    /* A worker that outlived the wait still calls SaveDone, which signals
       this event; closing it under that thread would hand SetEvent a handle
       that may since belong to something else. Leaving it to the process
       exit is the safe choice. */
    if (g_idle && InterlockedCompareExchange(&g_pending, 0, 0) == 0) {
        CloseHandle(g_idle);
        g_idle = NULL;
    }
    if (g_wic) {
        IWICImagingFactory_Release(g_wic);
        g_wic = NULL;
    }
}

/* ------------------------------------------------------------- encoding */

/*
 * One encoder for both formats; PNG and JPEG XR differ only in container,
 * pixel layout and one encoder option.
 *
 * The result is the stream's own HGLOBAL, trimmed to size. Handing that on
 * directly - to WriteFile, then to the clipboard, which takes ownership -
 * saves the two whole-file copies the earlier version made of every PNG.
 */
typedef enum { OPT_NONE, OPT_PNG_FILTER, OPT_JXR_QUALITY } EncodeOption;

static HGLOBAL Encode(IWICImagingFactory *wic, const GUID *container,
                      const WICPixelFormatGUID *pixfmt, EncodeOption opt,
                      int quality, const SnipImage *img, SIZE_T *sizeOut)
{
    IStream               *stream = NULL;
    IWICBitmapEncoder     *enc    = NULL;
    IWICBitmapFrameEncode *frame  = NULL;
    IPropertyBag2         *props  = NULL;
    WICPixelFormatGUID     fmt    = *pixfmt;
    HGLOBAL                mem    = NULL;
    HGLOBAL                out    = NULL;
    STATSTG                stat;

    *sizeOut = 0;
    if (!wic || !img || !img->pixels)
        return NULL;

    /* FALSE: the HGLOBAL outlives the stream and becomes the result. */
    if (FAILED(CreateStreamOnHGlobal(NULL, FALSE, &stream)))
        return NULL;
    if (FAILED(IWICImagingFactory_CreateEncoder(wic, container, NULL, &enc)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props)))
        goto done;

    if (props && opt != OPT_NONE) {
        PROPBAG2 bag;
        VARIANT  v;
        ZeroMemory(&bag, sizeof(bag));
        VariantInit(&v);
        if (opt == OPT_PNG_FILTER) {
            /*
             * Pinned to Sub rather than left on WIC's adaptive default: on an
             * 8-megapixel screenshot the adaptive search dominated the whole
             * snip, and Sub costs only a few per cent of file size on screen
             * content.
             */
            bag.pstrName = L"FilterOption";
            v.vt   = VT_UI1;
            v.bVal = WICPngFilterSub;
        } else if (quality >= 100) {
            bag.pstrName = L"Lossless";
            v.vt      = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
        } else {
            /*
             * Lossless is not a sensible default. On a busy 3840x2160 desktop
             * the in-box encoder takes about 19 seconds for it and produces a
             * 24 MB file; quality 0.90 takes a little over two and produces
             * 8 MB, and the difference is not visible on screen content.
             */
            bag.pstrName = L"ImageQuality";
            v.vt     = VT_R4;
            v.fltVal = (float)quality / 100.0f;
        }
        IPropertyBag2_Write(props, 1, &bag, &v);
    }

    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, props)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, img->width, img->height)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)))
        goto done;
    /* WIC negotiates: if it will not take our layout we would have to convert,
       and silently writing the wrong bytes is far worse than failing. */
    if (!IsEqualGUID(&fmt, pixfmt))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, img->height, img->stride,
                                                 img->stride * img->height,
                                                 img->pixels)))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)))
        goto done;
    if (FAILED(IWICBitmapEncoder_Commit(enc)))
        goto done;

    if (FAILED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) ||
        stat.cbSize.QuadPart == 0)
        goto done;
    if (FAILED(GetHGlobalFromStream(stream, &mem)) || !mem)
        goto done;

    /* The stream grows its block in steps, so it is usually larger than the
       data; a reader that goes by the block's size must not see the slack. */
    {
        HGLOBAL trimmed = GlobalReAlloc(mem, (SIZE_T)stat.cbSize.QuadPart, 0);
        if (trimmed)
            mem = trimmed;
    }
    out = mem;
    mem = NULL;
    *sizeOut = (SIZE_T)stat.cbSize.QuadPart;

done:
    if (props)  IPropertyBag2_Release(props);
    if (frame)  IWICBitmapFrameEncode_Release(frame);
    if (enc)    IWICBitmapEncoder_Release(enc);
    if (stream) {
        /* With fDeleteOnRelease FALSE the block is ours to free on failure. */
        if (!out && SUCCEEDED(GetHGlobalFromStream(stream, &mem)) && mem)
            GlobalFree(mem);
        IStream_Release(stream);
    }
    return out;
}

static HGLOBAL EncodePng(IWICImagingFactory *wic, const SnipImage *img, SIZE_T *size)
{
    return Encode(wic, &GUID_ContainerFormatPng, &GUID_WICPixelFormat32bppBGRA,
                  OPT_PNG_FILTER, 0, img, size);
}

/*
 * An HDR image as JPEG XR: 64bpp RGBA half in scRGB.
 *
 * This is what Xbox Game Bar writes for its HDR captures and what the Photos
 * app reads back as HDR, and it is the only floating-point format the in-box
 * WIC codecs can write at all - PNG has no float, and AVIF/HEIF need codecs
 * from the Store that may not be installed.
 */
static HGLOBAL EncodeJxr(IWICImagingFactory *wic, const SnipImage *img, int quality,
                         SIZE_T *size)
{
    if (!img || !img->hdr) {
        *size = 0;
        return NULL;
    }
    return Encode(wic, &GUID_ContainerFormatWmp, &GUID_WICPixelFormat64bppRGBAHalf,
                  OPT_JXR_QUALITY, quality, img, size);
}

/*
 * The SDR view of an HDR capture: everything that is not a file on disk needs
 * it, because Windows has no HDR clipboard format and no HDR DIB.
 */
static BOOL ToneMapToSdr(const SnipImage *hdr, SnipImage *sdr, BOOL rollOff)
{
    if (!hdr || !hdr->hdr || !Snip_AllocImage(sdr, hdr->width, hdr->height))
        return FALSE;
    Hdr_ToneMapScRgbToSrgb((const UINT16 *)hdr->pixels, hdr->stride / 2,
                           sdr->pixels, sdr->stride,
                           hdr->width, hdr->height, hdr->sdrWhiteNits, rollOff);
    return TRUE;
}

/* ------------------------------------------------------------- the file */

/*
 * Picks "Screenshot YYYY-MM-DD HHMMSS.<ext>" and *creates* it, with every
 * extension in 'exts' sharing one stem - an HDR snip and its tone-mapped
 * sidecar sort together and are obviously the same capture.
 *
 * Creating rather than checking is the point. Two snips inside the same second
 * are two workers running at once; with a PathFileExists test both found the
 * name free and the second overwrote the first. CREATE_NEW makes the file
 * system the referee. A stem only counts as free if every extension is.
 */
static BOOL ReserveFiles(const wchar_t *dir, const wchar_t *const *exts, int count,
                         HANDLE *handles, wchar_t (*paths)[MAX_PATH])
{
    SYSTEMTIME st;
    int        n, i;

    GetLocalTime(&st);

    for (n = 1; n <= 99; n++) {
        wchar_t base[MAX_PATH];
        HRESULT hr;
        BOOL    clash = FALSE;

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

        for (i = 0; i < count; i++)
            handles[i] = INVALID_HANDLE_VALUE;

        for (i = 0; i < count && !clash; i++) {
            if (FAILED(StringCchPrintfW(paths[i], MAX_PATH, L"%s.%s", base, exts[i])))
                return FALSE;
            handles[i] = CreateFileW(paths[i], GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
            if (handles[i] == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
                    Log_Printf(L"save: cannot create %s (%lu)", paths[i], err);
                    clash = TRUE;
                    n = 100;            /* not a name clash: give up */
                    break;
                }
                clash = TRUE;
            }
        }
        if (!clash)
            return TRUE;

        /* Hand back whatever this round created; it is empty and unwanted. */
        for (i = 0; i < count; i++) {
            if (handles[i] != INVALID_HANDLE_VALUE) {
                CloseHandle(handles[i]);
                DeleteFileW(paths[i]);
                handles[i] = INVALID_HANDLE_VALUE;
            }
        }
    }
    return FALSE;
}

/* Writes and closes. A half-written file is worse than none: it looks like a
   saved snip, so a failure removes it. */
static BOOL FinishFile(HANDLE f, const wchar_t *path, HGLOBAL data, SIZE_T size)
{
    const BYTE *p;
    DWORD       written = 0;
    BOOL        ok = FALSE;

    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    p = data ? (const BYTE *)GlobalLock(data) : NULL;
    if (p && size <= MAXDWORD)
        ok = WriteFile(f, p, (DWORD)size, &written, NULL) && written == size;
    if (p)
        GlobalUnlock(data);
    CloseHandle(f);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

BOOL Save_ImageToPath(const SnipImage *img, const wchar_t *path)
{
    HGLOBAL png;
    SIZE_T  size = 0;
    HANDLE  f;

    if (!img || !img->pixels || !path || !Save_Init())
        return FALSE;
    png = EncodePng(g_wic, img, &size);
    if (!png)
        return FALSE;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    {
        BOOL ok = FinishFile(f, path, png, size);
        GlobalFree(png);
        return ok;
    }
}

/* -------------------------------------------------------- the clipboard */

/* Any pixel short of opaque - only a freeform cut-out has them. */
static BOOL HasTransparency(const SnipImage *img)
{
    UINT32 y, x;
    for (y = 0; y < img->height; y++) {
        const BYTE *row = img->pixels + (SIZE_T)y * img->stride;
        for (x = 0; x < img->width; x++)
            if (row[x * 4 + 3] != 0xFF)
                return TRUE;
    }
    return FALSE;
}

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

/*
 * 'png' is handed over: the clipboard takes ownership of whatever it accepts,
 * and anything it does not take is freed here. The caller must not touch it
 * afterwards.
 */
static BOOL PutOnClipboard(HWND owner, const SnipImage *img, HGLOBAL png)
{
    HGLOBAL dib = NULL, dibv5;
    BOOL    any = FALSE;

    /*
     * CF_DIBV5 is always given; CF_DIB only when it has to differ from what
     * Windows would synthesise from it. For an opaque image - every rectangle,
     * window and full-screen snip - the system's own conversion is exactly
     * right, and building it here was a second full copy of the image (33 MB
     * for a 4K screen) for nothing. A freeform cut-out still gets its own,
     * composited over white.
     */
    dibv5 = MakeDib(img, TRUE);
    if (HasTransparency(img))
        dib = MakeDib(img, FALSE);
    if (!dib && !dibv5 && !png)
        return FALSE;

    if (!OpenClipboard(owner)) {
        /* Another process can hold the clipboard for a moment; one retry is
           enough in practice and keeps the capture from being lost. */
        Sleep(60);
        if (!OpenClipboard(owner)) {
            if (dib)   GlobalFree(dib);
            if (dibv5) GlobalFree(dibv5);
            if (png)   GlobalFree(png);
            return FALSE;
        }
    }
    EmptyClipboard();

    /* Most specific first - some apps take the first format they recognise. */
    if (png   && SetClipboardData(g_cfPng,   png))   { any = TRUE; png   = NULL; }
    if (dibv5 && SetClipboardData(CF_DIBV5,  dibv5)) { any = TRUE; dibv5 = NULL; }
    if (dib   && SetClipboardData(CF_DIB,    dib))   { any = TRUE; dib   = NULL; }

    CloseClipboard();

    /* Whatever the clipboard did not take is still ours to free. */
    if (dib)   GlobalFree(dib);
    if (dibv5) GlobalFree(dibv5);
    if (png)   GlobalFree(png);
    return any;
}

/* ------------------------------------------------------------ the entry */

/*
 * Everything a save needs, decided when the snip was taken: a settings
 * snapshot, so the settings dialog changing them meanwhile cannot reach a save
 * in progress, and this thread's own WIC factory.
 */
static BOOL SaveWith(const Settings *cfg, IWICImagingFactory *wic, HWND owner,
                     const SnipImage *img, SaveResult *res)
{
    SnipImage        sdr;
    const SnipImage *flat = img;     /* the 8-bit view of whatever came in */
    HGLOBAL          png  = NULL;
    HGLOBAL          jxr  = NULL;
    SIZE_T           pngSize = 0, jxrSize = 0;
    DWORD            t0;

    ZeroMemory(res, sizeof(*res));
    ZeroMemory(&sdr, sizeof(sdr));
    if (!img || !img->pixels || !wic)
        return FALSE;
    if (!g_cfPng)
        g_cfPng = RegisterClipboardFormatW(L"PNG");

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
        if (cfg->hdrSaveJxr && cfg->saveToDisk) {
            jxr = EncodeJxr(wic, img, cfg->hdrJxrQuality, &jxrSize);
            if (!jxr)
                Log_Printf(L"save: JPEG XR encode failed, falling back to PNG");
        }
        t2 = GetTickCount();
        flat = ToneMapToSdr(img, &sdr, cfg->hdrSdrRollOff) ? &sdr : NULL;
        Log_Printf(L"save: jxr %lu ms, tonemap %lu ms",
                   (unsigned long)(t2 - t1),
                   (unsigned long)(GetTickCount() - t2));
    }

    /* One PNG encode feeds the sidecar, the SDR file and the clipboard. */
    if (flat && (cfg->saveToDisk || cfg->copyToClipboard)) {
        DWORD t1 = GetTickCount();
        png = EncodePng(wic, flat, &pngSize);
        Log_Printf(L"save: png %lu ms", (unsigned long)(GetTickCount() - t1));
    }

    res->encodeMs = (long)(GetTickCount() - t0);
    res->hdr      = img->hdr;

    if (cfg->saveToDisk && (jxr || png)) {
        wchar_t dir[MAX_PATH];

        if (Settings_ResolveFolderFrom(cfg->saveFolder, FALSE, dir, ARRAYSIZE(dir))) {
            static const wchar_t *const both[] = { L"jxr", L"png" };
            static const wchar_t *const pngOnly[] = { L"png" };
            HANDLE  h[2];
            wchar_t paths[2][MAX_PATH];

            if (jxr) {
                /* The sidecar's name is reserved with the capture's, so the
                   pair can never land on a stem another snip already has. */
                int want = (png && cfg->hdrSdrSidecar) ? 2 : 1;
                if (ReserveFiles(dir, both, want, h, paths)) {
                    res->diskOk = FinishFile(h[0], paths[0], jxr, jxrSize);
                    StringCchCopyW(res->path, ARRAYSIZE(res->path), paths[0]);
                    if (want == 2) {
                        if (res->diskOk)
                            res->sidecarOk = FinishFile(h[1], paths[1], png, pngSize);
                        else {
                            CloseHandle(h[1]);
                            DeleteFileW(paths[1]);
                        }
                    }
                }
            } else if (ReserveFiles(dir, pngOnly, 1, h, paths)) {
                res->diskOk = FinishFile(h[0], paths[0], png, pngSize);
                StringCchCopyW(res->path, ARRAYSIZE(res->path), paths[0]);
            }
        }
        if (!res->diskOk)
            res->path[0] = 0;
    }

    /* The DIB formats do not need the PNG, so a failed encode still gets the
       image onto the clipboard. The clipboard takes the PNG block over. */
    if (cfg->copyToClipboard && flat) {
        res->clipboardOk = PutOnClipboard(owner, flat, png);
        png = NULL;
    }

    if (png) GlobalFree(png);
    if (jxr) GlobalFree(jxr);
    Snip_FreeImage(&sdr);
    return res->diskOk || res->clipboardOk;
}

static IWICImagingFactory *MakeFactory(void)
{
    IWICImagingFactory *wic = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&wic)))
        return NULL;
    return wic;
}

BOOL Save_Snip(HWND owner, const SnipImage *img, SaveResult *res)
{
    Settings            cfg = g_cfg;
    IWICImagingFactory *wic;
    BOOL                ok;

    if (!res)
        return FALSE;
    wic = MakeFactory();
    ok  = SaveWith(&cfg, wic, owner, img, res);
    if (wic)
        IWICImagingFactory_Release(wic);
    return ok;
}

/* ------------------------------------------------------- the worker */

typedef struct {
    HWND      notify;
    UINT      doneMsg;
    Settings  cfg;        /* as they were when the snip was taken */
    SnipImage img;        /* owned by the worker */
} SaveJob;

void Save_FreeResult(SaveResult *res)
{
    if (res)
        HeapFree(GetProcessHeap(), 0, res);
}

static unsigned __stdcall SaveThread(void *arg)
{
    SaveJob            *job = (SaveJob *)arg;
    SaveResult         *res;
    IWICImagingFactory *wic;

    /* WIC is COM, and this thread is not the one that initialised it. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wic = MakeFactory();

    res = (SaveResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*res));
    if (res)
        SaveWith(&job->cfg, wic, job->notify, &job->img, res);

    if (wic)
        IWICImagingFactory_Release(wic);
    Snip_FreeImage(&job->img);
    CoUninitialize();

    if (res && !PostMessageW(job->notify, job->doneMsg, 0, (LPARAM)res))
        Save_FreeResult(res);          /* the window went away first */

    HeapFree(GetProcessHeap(), 0, job);
    SaveDone();
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
    job->cfg     = g_cfg;
    job->img     = *img;
    ZeroMemory(img, sizeof(*img));     /* ownership has moved */

    /* Created on first use and reset here, so a wait that starts after this
       point cannot miss the thread that is about to run. */
    if (!g_idle)
        g_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (InterlockedIncrement(&g_pending) == 1 && g_idle)
        ResetEvent(g_idle);

    th = _beginthreadex(NULL, 0, SaveThread, job, 0, NULL);
    if (!th) {
        SaveDone();
        Snip_FreeImage(&job->img);
        HeapFree(GetProcessHeap(), 0, job);
        return FALSE;
    }
    CloseHandle((HANDLE)th);
    return TRUE;
}
