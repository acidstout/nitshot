/*
 * capture.c - DXGI Desktop Duplication with a GDI fallback.
 */

#define COBJMACROS

#include "nitshot.h"
#include "capture.h"
#include "settings.h"
#include "hdr.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>

/* How long a grab may spend coaxing a frame out of one output before the
   whole capture gives up and goes to GDI. A static desktop produces no new
   frames at all, so the first acquire usually times out and we then rebuild
   the duplication object - a fresh one always delivers the current desktop. */
#define WARM_TRIES      3
#define WARM_TIMEOUT_MS 20
#define FRESH_TRIES     10
#define FRESH_TIMEOUT_MS 30

typedef struct {
    ID3D11Device           *dev;
    ID3D11DeviceContext    *ctx;
    IDXGIOutput1           *out1;
    IDXGIOutput5           *out5;     /* NULL before Windows 10 1803        */
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D        *staging;
    UINT32                  stgW, stgH;
    RECT                    rect;      /* this output's desktop rectangle */
    BOOL                    holding;   /* a frame is acquired and unreleased */
    /*
     * Whether this duplication has ever delivered an actual desktop image.
     *
     * AcquireNextFrame also succeeds for a frame that only reports the mouse
     * pointer moving (LastPresentTime == 0). On a duplication that has handed
     * over an image before, that surface still holds it. On a fresh one it
     * holds nothing: every byte zero, black with zero alpha - which the
     * overlay's premultiplied blending then draws as nothing at all, so the
     * screen goes black and every selection rectangle stays behind as a trail.
     * Under remote desktop the pointer is reported moving all the time, and 11
     * cold grabs in 12 came back this way.
     */
    BOOL                    hasImage;
    HdrInfo                 hdr;
} CapOutput;

#define MAX_OUTPUTS 8

static CapOutput     g_out[MAX_OUTPUTS];
static int           g_outCount;
static BOOL          g_inited;
static BOOL          g_hdrMode;      /* duplicating in scRGB half-float       */
static float         g_sdrWhiteNits = 80.0f;
static const wchar_t *g_method = L"-";

/* ------------------------------------------------------------ image utils */

void Snip_FreeImage(SnipImage *img)
{
    if (!img)
        return;
    if (img->pixels)
        HeapFree(GetProcessHeap(), 0, img->pixels);
    ZeroMemory(img, sizeof(*img));
}

BOOL Snip_AllocImage(SnipImage *img, UINT32 width, UINT32 height)
{
    return Snip_AllocImageEx(img, width, height, FALSE);
}

BOOL Snip_AllocImageEx(SnipImage *img, UINT32 width, UINT32 height, BOOL hdr)
{
    SIZE_T bytes;

    ZeroMemory(img, sizeof(*img));
    if (width == 0 || height == 0 || width > 65535 || height > 65535)
        return FALSE;

    img->width        = width;
    img->height       = height;
    img->bpp          = hdr ? 8u : 4u;
    img->hdr          = hdr;
    img->sdrWhiteNits = 80.0f;
    img->stride       = width * img->bpp;
    bytes = (SIZE_T)img->stride * height;

    /* Zeroed, so gaps between monitors in the virtual desktop stay black
       instead of showing whatever was in the heap. */
    img->pixels = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!img->pixels) {
        ZeroMemory(img, sizeof(*img));
        return FALSE;
    }
    return TRUE;
}

BOOL Snip_CropImage(const SnipImage *src, const RECT *rc, SnipImage *dst)
{
    LONG   l, t, r, b;
    UINT32 y;

    if (!src || !src->pixels || !rc || !dst)
        return FALSE;

    l = max(rc->left,   0);
    t = max(rc->top,    0);
    r = min(rc->right,  (LONG)src->width);
    b = min(rc->bottom, (LONG)src->height);
    if (r <= l || b <= t)
        return FALSE;

    if (!Snip_AllocImageEx(dst, (UINT32)(r - l), (UINT32)(b - t), src->hdr))
        return FALSE;
    dst->sdrWhiteNits = src->sdrWhiteNits;

    for (y = 0; y < dst->height; y++) {
        memcpy(dst->pixels + (SIZE_T)y * dst->stride,
               src->pixels + (SIZE_T)(t + (LONG)y) * src->stride
                           + (SIZE_T)l * src->bpp,
               (SIZE_T)dst->width * src->bpp);
    }
    return TRUE;
}

/* --------------------------------------------------------- duplication */

static void ReleaseOutput(CapOutput *o)
{
    if (o->holding && o->dup) {
        IDXGIOutputDuplication_ReleaseFrame(o->dup);
        o->holding = FALSE;
    }
    if (o->staging) { ID3D11Texture2D_Release(o->staging);        o->staging = NULL; }
    if (o->dup)     { IDXGIOutputDuplication_Release(o->dup);     o->dup     = NULL; }
    if (o->out5)    { IDXGIOutput5_Release(o->out5);              o->out5    = NULL; }
    if (o->out1)    { IDXGIOutput1_Release(o->out1);              o->out1    = NULL; }
    if (o->ctx)     { ID3D11DeviceContext_Release(o->ctx);        o->ctx     = NULL; }
    if (o->dev)     { ID3D11Device_Release(o->dev);               o->dev     = NULL; }
    o->stgW = o->stgH = 0;
}

static BOOL RecreateDup(CapOutput *o)
{
    /* A new duplication starts with an empty surface, whatever the old one had. */
    o->hasImage = FALSE;
    if (o->holding) {
        IDXGIOutputDuplication_ReleaseFrame(o->dup);
        o->holding = FALSE;
    }
    if (o->dup) {
        IDXGIOutputDuplication_Release(o->dup);
        o->dup = NULL;
    }
    if (!o->dev)
        return FALSE;

    /*
     * DuplicateOutput1 is the only way to be handed the desktop in scRGB
     * half-float instead of the tone-mapped 8-bit version. It is also picky:
     * the process must be DPI-aware, which the manifest sees to.
     */
    if (g_hdrMode && o->out5) {
        DXGI_FORMAT want = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (SUCCEEDED(IDXGIOutput5_DuplicateOutput1(o->out5, (IUnknown *)o->dev,
                                                    0, 1, &want, &o->dup)))
            return TRUE;
        /* Fall through: better an SDR capture than none. */
        Log_Printf(L"capture: DuplicateOutput1(FLOAT) failed, using 8-bit");
    }
    if (!o->out1)
        return FALSE;
    return SUCCEEDED(IDXGIOutput1_DuplicateOutput(o->out1, (IUnknown *)o->dev, &o->dup));
}

static BOOL EnsureStaging(CapOutput *o, const D3D11_TEXTURE2D_DESC *src)
{
    D3D11_TEXTURE2D_DESC d;

    if (o->staging && o->stgW == src->Width && o->stgH == src->Height)
        return TRUE;
    if (o->staging) {
        ID3D11Texture2D_Release(o->staging);
        o->staging = NULL;
    }

    ZeroMemory(&d, sizeof(d));
    d.Width          = src->Width;
    d.Height         = src->Height;
    d.MipLevels      = 1;
    d.ArraySize      = 1;
    d.Format         = src->Format;
    d.SampleDesc.Count = 1;
    d.Usage          = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(ID3D11Device_CreateTexture2D(o->dev, &d, NULL, &o->staging))) {
        o->staging = NULL;
        return FALSE;
    }
    o->stgW = src->Width;
    o->stgH = src->Height;
    return TRUE;
}

static BOOL AcquireFrame(CapOutput *o, ID3D11Texture2D **tex)
{
    int attempt;

    *tex = NULL;

    for (attempt = 0; attempt < 2; attempt++) {
        int tries   = attempt == 0 ? WARM_TRIES   : FRESH_TRIES;
        int timeout = attempt == 0 ? WARM_TIMEOUT_MS : FRESH_TIMEOUT_MS;
        int i;

        for (i = 0; i < tries; i++) {
            DXGI_OUTDUPL_FRAME_INFO info;
            IDXGIResource *res = NULL;
            HRESULT hr;

            if (o->holding) {
                IDXGIOutputDuplication_ReleaseFrame(o->dup);
                o->holding = FALSE;
            }
            hr = IDXGIOutputDuplication_AcquireNextFrame(o->dup, (UINT)timeout, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                continue;
            if (FAILED(hr))
                break;             /* access lost, or the mode changed */

            o->holding = TRUE;

            /* A pointer-only frame on a duplication that has never shown an
               image carries nothing to capture; wait for one that does. See
               CapOutput.hasImage. */
            if (info.LastPresentTime.QuadPart != 0)
                o->hasImage = TRUE;
            else if (!o->hasImage) {
                IDXGIResource_Release(res);
                continue;
            }

            hr = IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)tex);
            IDXGIResource_Release(res);
            if (SUCCEEDED(hr))
                return TRUE;
            break;
        }

        if (!RecreateDup(o))
            return FALSE;
    }
    return FALSE;
}

/* Copy one output's frame into the virtual-desktop image at its own offset. */
static BOOL BlitOutput(CapOutput *o, SnipImage *img, const RECT *vb)
{
    DXGI_OUTDUPL_DESC       dd;
    D3D11_TEXTURE2D_DESC    td;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Texture2D        *frame = NULL;
    LONG    dx, dy;
    UINT32  w, h, y;
    BOOL    ok = FALSE;

    if (!o->dup)
        return FALSE;

    IDXGIOutputDuplication_GetDesc(o->dup, &dd);
    /* A rotated output hands out pixels in the panel's native orientation, not
       the desktop's. Rather than transpose here, the whole capture drops to
       GDI, which always reads desktop-oriented pixels. */
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        return FALSE;

    if (!AcquireFrame(o, &frame))
        return FALSE;

    ID3D11Texture2D_GetDesc(frame, &td);
    /* The frame must match what the composite image was allocated for; a
       mismatch means the display changed mode mid-grab, and the caller's
       wholesale fallback is the right answer. */
    if (td.Format != (img->hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                               : DXGI_FORMAT_B8G8R8A8_UNORM))
        goto done;
    if (!EnsureStaging(o, &td))
        goto done;

    ID3D11DeviceContext_CopyResource(o->ctx, (ID3D11Resource *)o->staging,
                                     (ID3D11Resource *)frame);
    if (FAILED(ID3D11DeviceContext_Map(o->ctx, (ID3D11Resource *)o->staging, 0,
                                       D3D11_MAP_READ, 0, &map)))
        goto done;

    dx = o->rect.left - vb->left;
    dy = o->rect.top  - vb->top;
    w  = td.Width;
    h  = td.Height;
    if (dx < 0 || dy < 0 ||
        (UINT32)dx + w > img->width || (UINT32)dy + h > img->height) {  /* NOLINT */
        /* The desktop changed shape between the metrics call and now. */
        ID3D11DeviceContext_Unmap(o->ctx, (ID3D11Resource *)o->staging, 0);
        goto done;
    }

    for (y = 0; y < h; y++) {
        memcpy(img->pixels + (SIZE_T)(dy + (LONG)y) * img->stride
                           + (SIZE_T)dx * img->bpp,
               (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
               (SIZE_T)w * img->bpp);
    }
    ID3D11DeviceContext_Unmap(o->ctx, (ID3D11Resource *)o->staging, 0);
    ok = TRUE;

done:
    if (frame)
        ID3D11Texture2D_Release(frame);
    if (o->holding) {
        IDXGIOutputDuplication_ReleaseFrame(o->dup);
        o->holding = FALSE;
    }
    return ok;
}

/* ---------------------------------------------------------- GDI fallback */

static BOOL GrabViaGdi(SnipImage *img, const RECT *vb)
{
    HDC        screen, mem;
    HBITMAP    dib, old;
    BITMAPINFO bi;
    void      *bits = NULL;
    BOOL       ok = FALSE;
    UINT32     y;
    int        w = (int)img->width, h = (int)img->height;

    screen = GetDC(NULL);
    if (!screen)
        return FALSE;
    mem = CreateCompatibleDC(screen);
    if (!mem) {
        ReleaseDC(NULL, screen);
        return FALSE;
    }

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          /* negative: top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib && bits) {
        old = (HBITMAP)SelectObject(mem, dib);
        /* CAPTUREBLT pulls in layered windows, which is what the user sees. */
        if (BitBlt(mem, 0, 0, w, h, screen, vb->left, vb->top, SRCCOPY | CAPTUREBLT)) {
            GdiFlush();
            memcpy(img->pixels, bits, (SIZE_T)img->stride * img->height);
            /* BitBlt leaves the alpha byte at zero; PNG and CF_DIBV5 both take
               that literally, so force the image opaque. */
            for (y = 0; y < img->height; y++) {
                BYTE  *p   = img->pixels + (SIZE_T)y * img->stride;
                UINT32 x;
                for (x = 0; x < img->width; x++)
                    p[x * 4 + 3] = 0xFF;
            }
            ok = TRUE;
        }
        SelectObject(mem, old);
    }
    if (dib)
        DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok;
}

/* ----------------------------------------------------------------- setup */

BOOL Capture_Init(void)
{
    IDXGIFactory1 *factory = NULL;
    UINT           ai;

    if (g_inited)
        return g_outCount > 0;
    g_inited = TRUE;

    /* Decided before any duplication object exists, because it picks their
       pixel format: one HDR display puts the whole composite into scRGB. */
    g_hdrMode      = !g_cfg.forceSdr && Hdr_AnyDisplayActive();
    g_sdrWhiteNits = 80.0f;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return FALSE;

    for (ai = 0; g_outCount < MAX_OUTPUTS; ai++) {
        IDXGIAdapter1 *adapter = NULL;
        UINT oi;

        if (IDXGIFactory1_EnumAdapters1(factory, ai, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        for (oi = 0; g_outCount < MAX_OUTPUTS; oi++) {
            IDXGIOutput    *output = NULL;
            DXGI_OUTPUT_DESC od;
            CapOutput      *o;
            D3D_FEATURE_LEVEL got;

            if (IDXGIAdapter1_EnumOutputs(adapter, oi, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(IDXGIOutput_GetDesc(output, &od)) || !od.AttachedToDesktop) {
                IDXGIOutput_Release(output);
                continue;
            }

            o = &g_out[g_outCount];
            ZeroMemory(o, sizeof(*o));
            o->rect = od.DesktopCoordinates;
            Hdr_QueryMonitor(od.DeviceName, &o->hdr);
            if (o->hdr.active)
                g_sdrWhiteNits = o->hdr.sdrWhiteNits;

            /* The device must live on the same adapter as the output, so this
               is created per adapter rather than once globally. */
            if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter,
                                         D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         NULL, 0, D3D11_SDK_VERSION,
                                         &o->dev, &got, &o->ctx))) {
                IDXGIOutput_Release(output);
                continue;
            }
            /* IDXGIOutput5 arrived in Windows 10 1803; without it there is no
               float duplication, only the 8-bit path. */
            IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput5, (void **)&o->out5);
            if (FAILED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1,
                                                  (void **)&o->out1)) ||
                !RecreateDup(o)) {
                ReleaseOutput(o);
                IDXGIOutput_Release(output);
                continue;
            }
            IDXGIOutput_Release(output);
            g_outCount++;
        }
        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory1_Release(factory);
    Log_Printf(L"capture: %d output(s), hdr=%d, sdr white %.0f nits",
               g_outCount, (int)g_hdrMode, g_sdrWhiteNits);
    return g_outCount > 0;
}

void Capture_Shutdown(void)
{
    int i;
    for (i = 0; i < g_outCount; i++)
        ReleaseOutput(&g_out[i]);
    g_outCount = 0;
    g_inited   = FALSE;
}

const wchar_t *Capture_LastMethod(void)
{
    return g_method;
}

BOOL Capture_VirtualDesktop(SnipImage *out, RECT *bounds)
{
    RECT vb;
    int  i;
    BOOL ok = TRUE;

    vb.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vb.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vb.right  = vb.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vb.bottom = vb.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    /* HDR can be switched on or off at any moment, and it decides the pixel
       format of every duplication object, so the machinery is rebuilt rather
       than left describing a display state that no longer exists. */
    if (!g_cfg.forceGdi) {
        BOOL wantHdr = !g_cfg.forceSdr && Hdr_AnyDisplayActive();
        if (g_inited && wantHdr != g_hdrMode) {
            Log_Printf(L"capture: advanced colour changed (%d -> %d), rebuilding",
                       (int)g_hdrMode, (int)wantHdr);
            Capture_Shutdown();
        }
        Capture_Init();
    }

    if (!Snip_AllocImageEx(out, (UINT32)(vb.right - vb.left),
                                (UINT32)(vb.bottom - vb.top),
                                !g_cfg.forceGdi && g_hdrMode && g_outCount > 0))
        return FALSE;
    out->sdrWhiteNits = g_sdrWhiteNits;
    if (bounds)
        *bounds = vb;

    /* One failing output invalidates the whole composite, so fall back
       wholesale rather than shipping a half-black desktop. */
    if (!g_cfg.forceGdi && g_outCount > 0) {
        for (i = 0; i < g_outCount; i++) {
            if (!BlitOutput(&g_out[i], out, &vb)) {
                ok = FALSE;
                break;
            }
        }
        if (ok) {
            g_method = out->hdr ? L"Desktop Duplication (HDR)"
                                : L"Desktop Duplication";
            return TRUE;
        }
    }

    /* GDI has no floating-point path at all, so falling back also means
       falling back to SDR - the image has to be reshaped for it. */
    if (out->hdr) {
        UINT32 w = out->width, h = out->height;
        Snip_FreeImage(out);
        if (!Snip_AllocImage(out, w, h))
            return FALSE;
    } else {
        ZeroMemory(out->pixels, (SIZE_T)out->stride * out->height);
    }

    if (GrabViaGdi(out, &vb)) {
        g_method = L"GDI";
        return TRUE;
    }

    Snip_FreeImage(out);
    return FALSE;
}
