/*
 * overlay.c - the frozen, dimmed desktop and the selection on top of it.
 *
 * Rendered with D3D11 rather than GDI so that phase 4 can switch the swap chain
 * to a floating-point format and show HDR content as it really looks. There is
 * no vertex buffer and no input layout anywhere: every draw is one quad built
 * from SV_VertexID, with the destination and source rectangles coming from a
 * constant buffer. Four shaders cover the whole UI.
 */

#define COBJMACROS

#include "nitshot.h"
#include "overlay.h"
#include "toolbar.h"
#include "poly.h"
#include "settings.h"
#include "save.h"
#include "hdr.h"
#include "log.h"

#include <windowsx.h>
#include <stdlib.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <shlwapi.h>      /* PathFindFileNameW, for naming who took focus */
#include <strsafe.h>

/* Points closer together than this are dropped while drawing freehand: it
   keeps the outline from collecting hundreds of duplicates during a slow drag. */
#define FREE_MIN_STEP   3
#define MAX_POLY_POINTS 8192

/* Watchdog: an overlay that somehow stops being the foreground window must
   take itself down. Left behind it would sit topmost over everything, which
   is the single worst way this program could fail. */
#define TIMER_WATCHDOG  1
#define WATCHDOG_MS     250

/*
 * How long after appearing the overlay defends its activation instead of
 * giving up on it.
 *
 * Measured from the log: the overlay was being closed 138 and 251 ms after it
 * appeared, with "lost activation", and no key and no second hotkey in
 * between. That is the moment the user lets go of the Windows key - the shell
 * reacts to the release and something takes activation back. Closing on it
 * looks exactly like a phantom Escape. Losing activation that early is never
 * the user switching away on purpose, so it is taken back instead; after this
 * window the overlay behaves as before, and a genuine switch still closes it.
 */
#define GRACE_MS        800
#define GRACE_RECLAIMS  4     /* never fight another window indefinitely */
#define WM_OV_RECLAIM   (WM_APP + 40)

/* ------------------------------------------------------------- shaders */

static const char g_hlsl[] =
"cbuffer CB : register(b0) {\n"
"    float4 dst;    // destination rectangle, NDC\n"
"    float4 src;    // source rectangle in the main texture, UV\n"
"    float4 src2;   // source rectangle in the mask texture, UV\n"
"    float4 tint;   // rgb multiplier or solid colour, a = alpha\n"
"};\n"
"Texture2D    tex  : register(t0);\n"
"Texture2D    msk  : register(t1);\n"
"SamplerState smp  : register(s0);\n"
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; };\n"
"VSOut VSMain(uint id : SV_VertexID) {\n"
"    float2 t = float2(id & 1, (id >> 1) & 1);\n"
"    VSOut o;\n"
"    o.pos = float4(lerp(dst.x, dst.z, t.x), lerp(dst.y, dst.w, t.y), 0, 1);\n"
"    o.uv  = float2(lerp(src.x,  src.z,  t.x), lerp(src.y,  src.w,  t.y));\n"
"    o.uv2 = float2(lerp(src2.x, src2.z, t.x), lerp(src2.y, src2.w, t.y));\n"
"    return o;\n"
"}\n"
/* Output is premultiplied throughout, so one blend state serves every pass. */
"float4 PSTex(VSOut i) : SV_Target {\n"
"    float4 c = tex.Sample(smp, i.uv);\n"
"    return float4(c.rgb * tint.rgb, c.a * tint.a);\n"
"}\n"
"float4 PSSolid(VSOut i) : SV_Target {\n"
"    return float4(tint.rgb * tint.a, tint.a);\n"
"}\n"
"float4 PSMask(VSOut i) : SV_Target {\n"
"    float  a = msk.Sample(smp, i.uv2).r;\n"
"    float4 c = tex.Sample(smp, i.uv);\n"
"    return float4(c.rgb * a, a);\n"
"}\n";

typedef struct {
    float dst[4];
    float src[4];
    float src2[4];
    float tint[4];
} ShaderCB;

/* --------------------------------------------------------------- state */

typedef struct {
    /* input */
    const SnipImage *frozen;
    RECT             bounds;        /* virtual desktop, screen coordinates */

    /* window and device */
    HWND                    hwnd;
    ID3D11Device           *dev;
    ID3D11DeviceContext    *ctx;
    IDXGISwapChain1        *swap;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader     *vs;
    ID3D11PixelShader      *psTex, *psSolid, *psMask;
    ID3D11Buffer           *cb;
    ID3D11BlendState       *blend;
    ID3D11SamplerState     *samp;
    ID3D11Texture2D        *texDesktop, *texToolbar, *texMask;
    ID3D11ShaderResourceView *srvDesktop, *srvToolbar, *srvMask;
    int                     toolbarW, toolbarH;

    /* interaction */
    Toolbar   tb;
    SnipMode  mode;
    BOOL      dragging;
    BOOL      hasRect;              /* a rectangle has been dragged out      */
    POINT     anchor, cursor;       /* image coordinates */
    POINT    *poly;
    int       polyCount, polyCap;
    BYTE     *maskBits;
    SIZE_T    maskCap;
    HWND      hoverWindow;
    RECT      hoverRect;            /* image coordinates */
    BOOL      recordMode;           /* the region will be recorded, not shot */
    BOOL      everActive;           /* seen WM_ACTIVATE at least once      */
    ULONGLONG shownAt;              /* GetTickCount64 when it appeared     */
    int       reclaims;             /* activation taken back in the grace  */
    BOOL      done, taken;
} Overlay;

static Overlay g_ov;

/* ------------------------------------------------------------- helpers */

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static int ImgW(void) { return (int)g_ov.frozen->width; }
static int ImgH(void) { return (int)g_ov.frozen->height; }
static BOOL IsHdr(void) { return g_ov.frozen->hdr; }

/* How much linear light SDR white is worth on this display. */
static float WhiteScale(void)
{
    float nits = g_ov.frozen->sdrWhiteNits;
    return (nits > 1.0f ? nits : 80.0f) / 80.0f;
}

/*
 * A UI colour, written as an ordinary sRGB value, put into whichever space the
 * render target is in. An scRGB target is linear and unbounded, so the same
 * number would come out far too bright if handed over unchanged.
 */
static float UiColour(float srgb)
{
    if (!IsHdr())
        return srgb;
    return Hdr_SrgbToLinear(srgb) * WhiteScale();
}

/* The dim is a multiplier, and multiplying encoded values by 0.6 is not the
   same as multiplying linear ones - matched here so both look alike. */
static float DimFactor(void)
{
    float d = 1.0f - (float)g_cfg.dimPercent / 100.0f;
    return IsHdr() ? Hdr_SrgbToLinear(d) : d;
}

static RECT NormRect(POINT a, POINT b)
{
    RECT r;
    r.left   = min(a.x, b.x);
    r.top    = min(a.y, b.y);
    r.right  = max(a.x, b.x);
    r.bottom = max(a.y, b.y);
    return r;
}

static void ClampToImage(RECT *r)
{
    if (r->left   < 0)       r->left   = 0;
    if (r->top    < 0)       r->top    = 0;
    if (r->right  > ImgW())  r->right  = ImgW();
    if (r->bottom > ImgH())  r->bottom = ImgH();
    if (r->right  < r->left) r->right  = r->left;
    if (r->bottom < r->top)  r->bottom = r->top;
}

/* The rectangle the current mode would capture, in image coordinates. */
static RECT CurrentRect(void)
{
    RECT r;

    switch (g_ov.mode) {
    case SNIP_FULLSCREEN:
        SetRect(&r, 0, 0, ImgW(), ImgH());
        break;
    case SNIP_WINDOW:
        r = g_ov.hoverRect;
        break;
    case SNIP_FREE:
        /*
         * Only an actual outline selects anything. There is no sensible
         * rectangle to fall back on before one exists: 'anchor' is still zero
         * until the first press, so falling back to it drew a selection from
         * the top-left corner of the desktop to the pointer, before the user
         * had clicked at all.
         */
        if (g_ov.polyCount >= 3)
            r = Poly_Bounds(g_ov.poly, g_ov.polyCount);
        else
            SetRect(&r, 0, 0, 0, 0);
        break;
    case SNIP_RECT:
    default:
        /* Deliberately not keyed on 'dragging': the rectangle has to survive
           the mouse-up that commits it, which is when it is read back. */
        if (g_ov.hasRect)
            r = NormRect(g_ov.anchor, g_ov.cursor);
        else
            SetRect(&r, 0, 0, 0, 0);
        break;
    }
    ClampToImage(&r);
    return r;
}

/* --------------------------------------------------------- window mode */

typedef struct { POINT pt; HWND self; HWND found; RECT rect; } HitCtx;

static BOOL IsCloaked(HWND h)
{
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return FALSE;
}

/* DWM's frame bounds exclude the invisible resize border, so the highlight
   matches what the user sees instead of being a few pixels too big. */
static RECT FrameBounds(HWND h)
{
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

static BOOL CALLBACK HitProc(HWND h, LPARAM lp)
{
    HitCtx *c = (HitCtx *)lp;
    RECT    r;

    if (h == c->self || !IsWindowVisible(h) || IsIconic(h) || IsCloaked(h))
        return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT)
        return TRUE;

    r = FrameBounds(h);
    if (r.right - r.left < 8 || r.bottom - r.top < 8)
        return TRUE;
    if (!PtInRect(&r, c->pt))
        return TRUE;

    /* EnumWindows walks front to back, so the first hit is the top one. */
    c->found = h;
    c->rect  = r;
    return FALSE;
}

static void UpdateHoverWindow(POINT screenPt)
{
    HitCtx c;

    ZeroMemory(&c, sizeof(c));
    c.pt   = screenPt;
    c.self = g_ov.hwnd;
    EnumWindows(HitProc, (LPARAM)&c);

    g_ov.hoverWindow = c.found;
    if (c.found) {
        OffsetRect(&c.rect, -g_ov.bounds.left, -g_ov.bounds.top);
        g_ov.hoverRect = c.rect;
        ClampToImage(&g_ov.hoverRect);
    } else {
        SetRectEmpty(&g_ov.hoverRect);
    }
}

/* -------------------------------------------------------- the freeform */

static BOOL PolyPush(POINT p)
{
    if (g_ov.polyCount >= MAX_POLY_POINTS)
        return FALSE;
    if (g_ov.polyCount == g_ov.polyCap) {
        int    cap = g_ov.polyCap ? g_ov.polyCap * 2 : 256;
        POINT *n   = (POINT *)(g_ov.poly
                     ? HeapReAlloc(GetProcessHeap(), 0, g_ov.poly, (SIZE_T)cap * sizeof(POINT))
                     : HeapAlloc(GetProcessHeap(), 0, (SIZE_T)cap * sizeof(POINT)));
        if (!n)
            return FALSE;
        g_ov.poly   = n;
        g_ov.polyCap = cap;
    }
    g_ov.poly[g_ov.polyCount++] = p;
    return TRUE;
}

static void PolyReset(void)
{
    g_ov.polyCount = 0;
}

/* -------------------------------------------------------------- device */

static BOOL CompileShaders(void)
{
    HMODULE lib;
    pD3DCompile compile;
    ID3DBlob   *vsb = NULL, *psa = NULL, *psb = NULL, *psc = NULL, *err = NULL;
    BOOL        ok = FALSE;
    const UINT  flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    /* Loaded by name rather than linked: the import library is versioned and
       the DLL is in System32 on every supported Windows. */
    lib = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!lib)
        return FALSE;
    compile = (pD3DCompile)(void *)GetProcAddress(lib, "D3DCompile");
    if (!compile)
        return FALSE;

    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "VSMain", "vs_4_0", flags, 0, &vsb, &err))) goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSTex", "ps_4_0", flags, 0, &psa, &err)))  goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSSolid", "ps_4_0", flags, 0, &psb, &err))) goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSMask", "ps_4_0", flags, 0, &psc, &err)))  goto done;

    if (FAILED(ID3D11Device_CreateVertexShader(g_ov.dev,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
            NULL, &g_ov.vs))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(g_ov.dev,
            ID3D10Blob_GetBufferPointer(psa), ID3D10Blob_GetBufferSize(psa),
            NULL, &g_ov.psTex))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(g_ov.dev,
            ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
            NULL, &g_ov.psSolid))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(g_ov.dev,
            ID3D10Blob_GetBufferPointer(psc), ID3D10Blob_GetBufferSize(psc),
            NULL, &g_ov.psMask))) goto done;
    ok = TRUE;

done:
    if (!ok && err)
        Log_Printf(L"overlay: shader compile failed: %S",
                   (const char *)ID3D10Blob_GetBufferPointer(err));
    RELEASE(err);
    RELEASE(vsb);
    RELEASE(psa);
    RELEASE(psb);
    RELEASE(psc);
    return ok;
}

static BOOL CreateDesktopTexture(void)
{
    D3D11_TEXTURE2D_DESC    d;
    D3D11_SUBRESOURCE_DATA  init;

    ZeroMemory(&d, sizeof(d));
    d.Width            = g_ov.frozen->width;
    d.Height           = g_ov.frozen->height;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = IsHdr() ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_IMMUTABLE;
    d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    ZeroMemory(&init, sizeof(init));
    init.pSysMem     = g_ov.frozen->pixels;
    init.SysMemPitch = g_ov.frozen->stride;

    if (FAILED(ID3D11Device_CreateTexture2D(g_ov.dev, &d, &init, &g_ov.texDesktop)))
        return FALSE;
    return SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_ov.dev,
                (ID3D11Resource *)g_ov.texDesktop, NULL, &g_ov.srvDesktop));
}

static BOOL EnsureMaskTexture(void)
{
    D3D11_TEXTURE2D_DESC d;

    if (g_ov.texMask)
        return TRUE;

    ZeroMemory(&d, sizeof(d));
    d.Width            = g_ov.frozen->width;
    d.Height           = g_ov.frozen->height;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    /* DEFAULT rather than DYNAMIC: UpdateSubresource can write just the
       outline's bounding box, where a Map would discard the whole surface. */
    d.Usage            = D3D11_USAGE_DEFAULT;
    d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(ID3D11Device_CreateTexture2D(g_ov.dev, &d, NULL, &g_ov.texMask)))
        return FALSE;
    return SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_ov.dev,
                (ID3D11Resource *)g_ov.texMask, NULL, &g_ov.srvMask));
}

static BOOL UploadToolbar(void)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE map;
    int   w = g_ov.tb.rect.right  - g_ov.tb.rect.left;
    int   h = g_ov.tb.rect.bottom - g_ov.tb.rect.top;
    BYTE *tmp;
    BOOL  ok = FALSE;

    if (w <= 0 || h <= 0)
        return FALSE;

    if (!g_ov.texToolbar || g_ov.toolbarW != w || g_ov.toolbarH != h) {
        RELEASE(g_ov.srvToolbar);
        RELEASE(g_ov.texToolbar);

        ZeroMemory(&d, sizeof(d));
        d.Width            = (UINT)w;
        d.Height           = (UINT)h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = IsHdr() ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                     : DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DYNAMIC;
        d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_ov.dev, &d, NULL, &g_ov.texToolbar)))
            return FALSE;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_ov.dev,
                (ID3D11Resource *)g_ov.texToolbar, NULL, &g_ov.srvToolbar)))
            return FALSE;
        g_ov.toolbarW = w;
        g_ov.toolbarH = h;
    }

    tmp = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    if (!tmp)
        return FALSE;
    if (Toolbar_Render(&g_ov.tb, tmp, w * 4) &&
        SUCCEEDED(ID3D11DeviceContext_Map(g_ov.ctx, (ID3D11Resource *)g_ov.texToolbar,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        int y;
        for (y = 0; y < h; y++) {
            const BYTE *srcRow = tmp + (SIZE_T)y * w * 4;
            BYTE       *dstRow = (BYTE *)map.pData + (SIZE_T)y * map.RowPitch;

            if (!IsHdr()) {
                memcpy(dstRow, srcRow, (SIZE_T)w * 4);
                continue;
            }
            /*
             * The toolbar is drawn as premultiplied 8-bit sRGB; scRGB wants
             * premultiplied linear. Converting has to go through the
             * unpremultiplied colour, or the anti-aliased edges - the whole
             * point of drawing it at 4x - come out the wrong brightness.
             */
            {
                UINT16 *d16   = (UINT16 *)dstRow;
                float   white = WhiteScale();
                int     x;
                for (x = 0; x < w; x++) {
                    float a = srcRow[x * 4 + 3] / 255.0f;
                    int   i;
                    for (i = 0; i < 3; i++) {
                        /* source is BGRA, destination RGBA */
                        float c = srcRow[x * 4 + (2 - i)] / 255.0f;
                        float lin;
                        if (a > 0.0f) {
                            c /= a;
                            if (c > 1.0f)
                                c = 1.0f;
                        }
                        lin = Hdr_SrgbToLinear(c) * white * a;
                        d16[x * 4 + i] = Hdr_FloatToHalf(lin);
                    }
                    d16[x * 4 + 3] = Hdr_FloatToHalf(a);
                }
            }
        }
        ID3D11DeviceContext_Unmap(g_ov.ctx, (ID3D11Resource *)g_ov.texToolbar, 0);
        ok = TRUE;
    }
    HeapFree(GetProcessHeap(), 0, tmp);
    return ok;
}

/* Rasterise the current outline and push just its bounding box to the GPU. */
static void UploadMask(const RECT *box)
{
    D3D11_BOX b;
    SIZE_T    need;
    int       w = box->right - box->left;
    int       h = box->bottom - box->top;

    if (w <= 0 || h <= 0 || !EnsureMaskTexture())
        return;

    need = (SIZE_T)w * h;
    if (need > g_ov.maskCap) {
        BYTE *n = (BYTE *)(g_ov.maskBits
                  ? HeapReAlloc(GetProcessHeap(), 0, g_ov.maskBits, need)
                  : HeapAlloc(GetProcessHeap(), 0, need));
        if (!n)
            return;
        g_ov.maskBits = n;
        g_ov.maskCap  = need;
    }

    Poly_RasterizeMask(g_ov.poly, g_ov.polyCount, box, g_ov.maskBits, w);

    b.left   = (UINT)box->left;
    b.top    = (UINT)box->top;
    b.front  = 0;
    b.right  = (UINT)box->right;
    b.bottom = (UINT)box->bottom;
    b.back   = 1;
    ID3D11DeviceContext_UpdateSubresource(g_ov.ctx, (ID3D11Resource *)g_ov.texMask,
                                          0, &b, g_ov.maskBits, (UINT)w, 0);
}

static BOOL CreateDevice(void)
{
    DXGI_SWAP_CHAIN_DESC1 sd;
    D3D_FEATURE_LEVEL     fl;
    IDXGIDevice          *dxgiDev = NULL;
    IDXGIAdapter         *adapter = NULL;
    IDXGIFactory2        *factory = NULL;
    ID3D11Texture2D      *back    = NULL;
    D3D11_BLEND_DESC      bd;
    D3D11_SAMPLER_DESC    sm;
    D3D11_BUFFER_DESC     cbd;
    BOOL                  ok = FALSE;

    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                 D3D11_SDK_VERSION, &g_ov.dev, &fl, &g_ov.ctx)))
        goto done;

    if (FAILED(ID3D11Device_QueryInterface(g_ov.dev, &IID_IDXGIDevice, (void **)&dxgiDev)))
        goto done;
    if (FAILED(IDXGIDevice_GetAdapter(dxgiDev, &adapter)))
        goto done;
    if (FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory)))
        goto done;

    ZeroMemory(&sd, sizeof(sd));
    sd.Width            = g_ov.frozen->width;
    sd.Height           = g_ov.frozen->height;
    sd.Format           = IsHdr() ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                  : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.Scaling          = DXGI_SCALING_NONE;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_ov.dev,
                                                    g_ov.hwnd, &sd, NULL, NULL,
                                                    &g_ov.swap)))
        goto done;
    /* Alt+Enter would try to take this borderless window fullscreen. */
    IDXGIFactory2_MakeWindowAssociation(factory, g_ov.hwnd, DXGI_MWA_NO_ALT_ENTER);

    /*
     * A float swap chain means nothing on its own - DWM has to be told the
     * numbers are scRGB, or it treats them as sRGB and the whole overlay comes
     * out washed out. Asked for rather than assumed: if the compositor will
     * not take it, the frozen desktop is still shown, just tone-mapped.
     */
    if (IsHdr()) {
        IDXGISwapChain3 *sc3 = NULL;
        if (SUCCEEDED(IDXGISwapChain1_QueryInterface(g_ov.swap, &IID_IDXGISwapChain3,
                                                     (void **)&sc3))) {
            UINT support = 0;
            if (SUCCEEDED(IDXGISwapChain3_CheckColorSpaceSupport(sc3,
                    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                IDXGISwapChain3_SetColorSpace1(sc3,
                    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            } else {
                Log_Printf(L"overlay: scRGB colour space refused by the compositor");
            }
            IDXGISwapChain3_Release(sc3);
        }
    }

    if (FAILED(IDXGISwapChain1_GetBuffer(g_ov.swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
        goto done;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_ov.dev, (ID3D11Resource *)back,
                                                   NULL, &g_ov.rtv)))
        goto done;

    if (!CompileShaders() || !CreateDesktopTexture())
        goto done;

    ZeroMemory(&bd, sizeof(bd));
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;   /* premultiplied */
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_ov.dev, &bd, &g_ov.blend)))
        goto done;

    ZeroMemory(&sm, sizeof(sm));
    sm.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;   /* 1:1, so no filtering */
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_ov.dev, &sm, &g_ov.samp)))
        goto done;

    ZeroMemory(&cbd, sizeof(cbd));
    cbd.ByteWidth      = sizeof(ShaderCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_ov.dev, &cbd, NULL, &g_ov.cb)))
        goto done;

    ok = TRUE;

done:
    RELEASE(back);
    RELEASE(factory);
    RELEASE(adapter);
    RELEASE(dxgiDev);
    return ok;
}

static void DestroyDevice(void)
{
    RELEASE(g_ov.srvMask);
    RELEASE(g_ov.texMask);
    RELEASE(g_ov.srvToolbar);
    RELEASE(g_ov.texToolbar);
    RELEASE(g_ov.srvDesktop);
    RELEASE(g_ov.texDesktop);
    RELEASE(g_ov.cb);
    RELEASE(g_ov.samp);
    RELEASE(g_ov.blend);
    RELEASE(g_ov.psMask);
    RELEASE(g_ov.psSolid);
    RELEASE(g_ov.psTex);
    RELEASE(g_ov.vs);
    RELEASE(g_ov.rtv);
    RELEASE(g_ov.swap);
    RELEASE(g_ov.ctx);
    RELEASE(g_ov.dev);
}

/* ------------------------------------------------------------ rendering */

static void SetCB(const RECT *dst, const RECT *src, const RECT *src2,
                  float r, float g, float b, float a)
{
    D3D11_MAPPED_SUBRESOURCE m;
    ShaderCB cb;
    float    W = (float)ImgW(), H = (float)ImgH();

    cb.dst[0] = (float)dst->left   / W * 2.0f - 1.0f;
    cb.dst[1] = 1.0f - (float)dst->top    / H * 2.0f;
    cb.dst[2] = (float)dst->right  / W * 2.0f - 1.0f;
    cb.dst[3] = 1.0f - (float)dst->bottom / H * 2.0f;

    if (src) {
        cb.src[0] = (float)src->left   / W;
        cb.src[1] = (float)src->top    / H;
        cb.src[2] = (float)src->right  / W;
        cb.src[3] = (float)src->bottom / H;
    } else {
        cb.src[0] = cb.src[1] = 0.0f;
        cb.src[2] = cb.src[3] = 1.0f;
    }
    if (src2) {
        cb.src2[0] = (float)src2->left   / W;
        cb.src2[1] = (float)src2->top    / H;
        cb.src2[2] = (float)src2->right  / W;
        cb.src2[3] = (float)src2->bottom / H;
    } else {
        cb.src2[0] = cb.src2[1] = 0.0f;
        cb.src2[2] = cb.src2[3] = 1.0f;
    }
    cb.tint[0] = r; cb.tint[1] = g; cb.tint[2] = b; cb.tint[3] = a;

    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ov.ctx, (ID3D11Resource *)g_ov.cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(g_ov.ctx, (ID3D11Resource *)g_ov.cb, 0);
    }
}

static void DrawQuad(void)
{
    ID3D11DeviceContext_Draw(g_ov.ctx, 4, 0);
}

static void FillRectSolid(const RECT *r, float cr, float cg, float cb, float a)
{
    if (r->right <= r->left || r->bottom <= r->top)
        return;
    ID3D11DeviceContext_PSSetShader(g_ov.ctx, g_ov.psSolid, NULL, 0);
    SetCB(r, NULL, NULL, UiColour(cr), UiColour(cg), UiColour(cb), a);
    DrawQuad();
}

/* A 'width'-pixel frame just outside 'r'. */
static void StrokeRect(const RECT *r, int width, float cr, float cg, float cb, float a)
{
    RECT e;

    e.left = r->left - width; e.top = r->top - width;
    e.right = r->right + width; e.bottom = r->top;
    FillRectSolid(&e, cr, cg, cb, a);

    e.top = r->bottom; e.bottom = r->bottom + width;
    FillRectSolid(&e, cr, cg, cb, a);

    e.left = r->left - width; e.right = r->left;
    e.top = r->top; e.bottom = r->bottom;
    FillRectSolid(&e, cr, cg, cb, a);

    e.left = r->right; e.right = r->right + width;
    FillRectSolid(&e, cr, cg, cb, a);
}

static void Render(void)
{
    RECT  full, sel, tbRect;
    float dim = DimFactor();
    UINT  stride = 0;

    if (!g_ov.rtv)
        return;

    SetRect(&full, 0, 0, ImgW(), ImgH());
    sel = CurrentRect();

    ID3D11DeviceContext_OMSetRenderTargets(g_ov.ctx, 1, &g_ov.rtv, NULL);
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width  = (float)ImgW();
        vp.Height = (float)ImgH();
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_ov.ctx, 1, &vp);
    }
    ID3D11DeviceContext_OMSetBlendState(g_ov.ctx, g_ov.blend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ov.ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(g_ov.ctx, NULL);
    ID3D11DeviceContext_IASetVertexBuffers(g_ov.ctx, 0, 0, NULL, &stride, &stride);
    ID3D11DeviceContext_VSSetShader(g_ov.ctx, g_ov.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ov.ctx, 0, 1, &g_ov.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ov.ctx, 0, 1, &g_ov.cb);
    ID3D11DeviceContext_PSSetSamplers(g_ov.ctx, 0, 1, &g_ov.samp);

    /* 1. the whole desktop, dimmed */
    ID3D11DeviceContext_PSSetShader(g_ov.ctx, g_ov.psTex, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ov.ctx, 0, 1, &g_ov.srvDesktop);
    SetCB(&full, &full, NULL, dim, dim, dim, 1.0f);
    DrawQuad();

    /*
     * 2. the selection, at full brightness. Freeform is never drawn as a
     * rectangle, not even for the first point or two of a stroke - the shape
     * is the selection, and a rectangle flashing up before it would say the
     * wrong thing about what is about to be captured.
     */
    if (g_ov.mode == SNIP_FREE) {
        RECT box = Poly_Bounds(g_ov.poly, g_ov.polyCount);
        ClampToImage(&box);
        if (g_ov.polyCount >= 3 && g_ov.srvMask &&
            box.right > box.left && box.bottom > box.top) {
            ID3D11ShaderResourceView *srvs[2];
            srvs[0] = g_ov.srvDesktop;
            srvs[1] = g_ov.srvMask;
            ID3D11DeviceContext_PSSetShaderResources(g_ov.ctx, 0, 2, srvs);
            ID3D11DeviceContext_PSSetShader(g_ov.ctx, g_ov.psMask, NULL, 0);
            SetCB(&box, &box, &box, 1.0f, 1.0f, 1.0f, 1.0f);
            DrawQuad();
            ID3D11DeviceContext_PSSetShaderResources(g_ov.ctx, 0, 1, &g_ov.srvDesktop);
        }
    } else if (sel.right > sel.left && sel.bottom > sel.top) {
        ID3D11DeviceContext_PSSetShader(g_ov.ctx, g_ov.psTex, NULL, 0);
        SetCB(&sel, &sel, NULL, 1.0f, 1.0f, 1.0f, 1.0f);
        DrawQuad();
    }

    /* 3. the outline */
    if (g_ov.mode == SNIP_FREE) {
        int i;
        /* The path itself, as a chain of small squares - at these widths a
           proper stroke would not look any different. */
        for (i = 0; i < g_ov.polyCount; i++) {
            RECT d;
            SetRect(&d, g_ov.poly[i].x - 1, g_ov.poly[i].y - 1,
                        g_ov.poly[i].x + 2, g_ov.poly[i].y + 2);
            FillRectSolid(&d, 0.30f, 0.76f, 1.0f, 1.0f);
        }
    } else if (sel.right > sel.left && sel.bottom > sel.top) {
        StrokeRect(&sel, 2, 0.30f, 0.76f, 1.0f, 1.0f);
    }

    /* 4. the toolbar */
    if (g_ov.srvToolbar) {
        tbRect = g_ov.tb.rect;
        OffsetRect(&tbRect, -g_ov.bounds.left, -g_ov.bounds.top);
        ID3D11DeviceContext_PSSetShader(g_ov.ctx, g_ov.psTex, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ov.ctx, 0, 1, &g_ov.srvToolbar);
        SetCB(&tbRect, NULL, NULL, 1.0f, 1.0f, 1.0f, 1.0f);
        DrawQuad();
    }

    IDXGISwapChain1_Present(g_ov.swap, 0, 0);
}

/*
 * Read the back buffer back and write it to a PNG. Only reachable with
 * DebugLog on: an overlay drawn into a flip-model swap chain is invisible to
 * every ordinary screen-capture route (BitBlt and PrintWindow both return the
 * desktop underneath it), so this is the only way to look at what it rendered.
 */
static void DumpBackBuffer(void)
{
    ID3D11Texture2D         *back = NULL, *stage = NULL;
    D3D11_TEXTURE2D_DESC     d;
    D3D11_MAPPED_SUBRESOURCE map;
    SnipImage                img;
    wchar_t                  dir[MAX_PATH], path[MAX_PATH];

    if (FAILED(IDXGISwapChain1_GetBuffer(g_ov.swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
        return;
    ID3D11Texture2D_GetDesc(back, &d);
    d.Usage          = D3D11_USAGE_STAGING;
    d.BindFlags      = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags      = 0;

    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_ov.dev, &d, NULL, &stage))) {
        ID3D11DeviceContext_CopyResource(g_ov.ctx, (ID3D11Resource *)stage,
                                         (ID3D11Resource *)back);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ov.ctx, (ID3D11Resource *)stage, 0,
                                              D3D11_MAP_READ, 0, &map))) {
            if (Snip_AllocImageEx(&img, d.Width, d.Height, IsHdr())) {
                UINT32 y;
                img.sdrWhiteNits = g_ov.frozen->sdrWhiteNits;
                for (y = 0; y < d.Height; y++)
                    memcpy(img.pixels + (SIZE_T)y * img.stride,
                           (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
                           img.stride);
                /* The dump is for looking at, so an HDR back buffer is
                   tone-mapped rather than written as a .jxr. */
                if (img.hdr) {
                    SnipImage sdr;
                    if (Snip_AllocImage(&sdr, img.width, img.height)) {
                        Hdr_ToneMapScRgbToSrgb((const UINT16 *)img.pixels,
                                               img.stride / 2, sdr.pixels,
                                               sdr.stride, img.width, img.height,
                                               img.sdrWhiteNits,
                                               g_cfg.hdrSdrRollOff);
                        Snip_FreeImage(&img);
                        img = sdr;
                    }
                }
                if (Settings_ResolveSaveFolder(dir, ARRAYSIZE(dir))) {
                    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\overlay-debug.png", dir);
                    Log_Printf(L"overlay: back buffer dumped to %s (%d)",
                               path, (int)Save_ImageToPath(&img, path));
                }
                Snip_FreeImage(&img);
            }
            ID3D11DeviceContext_Unmap(g_ov.ctx, (ID3D11Resource *)stage, 0);
        }
    }
    RELEASE(stage);
    RELEASE(back);
}

/* --------------------------------------------------------------- input */

/* Defined with the foreground handling further down. */
static BOOL ReclaimActivation(HWND hwnd, HWND thief, const wchar_t *how);
static void ForceForeground(HWND hwnd);

static void Finish(BOOL taken, const wchar_t *why)
{
    if (g_ov.done)
        return;
    Log_Printf(L"overlay: finish taken=%d (%s) mode=%d poly=%d",
               (int)taken, why, (int)g_ov.mode, g_ov.polyCount);
    g_ov.taken = taken;
    g_ov.done  = TRUE;
    PostMessageW(g_ov.hwnd, WM_CLOSE, 0, 0);
}

/*
 * Recording reuses the rectangle mode - what changes is what happens to the
 * region afterwards, so the toolbar shows Record as the active button and the
 * selection carries a flag rather than there being a whole extra mode.
 */
static void SetRecordMode(void)
{
    g_ov.recordMode = TRUE;
    g_ov.mode       = SNIP_RECT;
    g_ov.dragging   = FALSE;
    g_ov.hasRect    = FALSE;
    PolyReset();
    g_ov.tb.active  = TB_RECORD;
    UploadToolbar();
    Render();
}

static void SetMode(SnipMode m)
{
    if (g_ov.mode == m && !g_ov.recordMode)
        return;
    g_ov.recordMode = FALSE;
    g_ov.mode     = m;
    g_ov.dragging = FALSE;
    g_ov.hasRect  = FALSE;
    PolyReset();
    g_ov.tb.active = (m == SNIP_RECT)   ? TB_RECT :
                     (m == SNIP_FREE)   ? TB_FREE :
                     (m == SNIP_WINDOW) ? TB_WINDOW : TB_FULL;
    UploadToolbar();
    if (m == SNIP_WINDOW) {
        POINT screen = g_ov.cursor;
        screen.x += g_ov.bounds.left;
        screen.y += g_ov.bounds.top;
        UpdateHoverWindow(screen);
    }
    Render();
}

static void OnMouseMove(POINT img)
{
    POINT screen = img;
    int   hot;

    screen.x += g_ov.bounds.left;
    screen.y += g_ov.bounds.top;
    g_ov.cursor = img;

    hot = g_ov.dragging ? -1 : Toolbar_HitTest(&g_ov.tb, screen);
    if (hot != g_ov.tb.hot) {
        g_ov.tb.hot = hot;
        UploadToolbar();
    }

    if (g_ov.dragging) {
        if (g_ov.mode == SNIP_FREE) {
            POINT last = g_ov.polyCount ? g_ov.poly[g_ov.polyCount - 1] : img;
            if (!g_ov.polyCount ||
                abs(img.x - last.x) >= FREE_MIN_STEP ||
                abs(img.y - last.y) >= FREE_MIN_STEP) {
                PolyPush(img);
                if (g_ov.polyCount > 2) {
                    RECT box = Poly_Bounds(g_ov.poly, g_ov.polyCount);
                    ClampToImage(&box);
                    UploadMask(&box);
                }
            }
        }
    } else if (g_ov.mode == SNIP_WINDOW) {
        UpdateHoverWindow(screen);
    }
    Render();
}

static void OnLeftDown(POINT img)
{
    POINT screen = img;
    int   hit;

    Log_Printf(L"overlay: down at %d,%d mode=%d", img.x, img.y, (int)g_ov.mode);

    screen.x += g_ov.bounds.left;
    screen.y += g_ov.bounds.top;

    hit = Toolbar_HitTest(&g_ov.tb, screen);
    if (hit >= 0) {
        switch (hit) {
        case TB_RECT:   SetMode(SNIP_RECT);   break;
        case TB_FREE:   SetMode(SNIP_FREE);   break;
        case TB_WINDOW: SetMode(SNIP_WINDOW); break;
        case TB_FULL:   g_ov.mode = SNIP_FULLSCREEN; Finish(TRUE, L"toolbar fullscreen"); break;
        case TB_RECORD: SetRecordMode(); break;
        case TB_CLOSE:  Finish(FALSE, L"toolbar close"); break;
        default: break;
        }
        return;
    }
    /* A click on the pill but not on a button must not start a drag. */
    if (Toolbar_Contains(&g_ov.tb, screen))
        return;

    switch (g_ov.mode) {
    case SNIP_WINDOW:
        UpdateHoverWindow(screen);
        if (g_ov.hoverWindow)
            Finish(TRUE, L"window picked");
        break;
    case SNIP_FULLSCREEN:
        Finish(TRUE, L"fullscreen click");
        break;
    case SNIP_FREE:
        PolyReset();
        g_ov.dragging = TRUE;
        g_ov.anchor   = img;
        PolyPush(img);
        SetCapture(g_ov.hwnd);
        break;
    case SNIP_RECT:
    default:
        g_ov.dragging = TRUE;
        g_ov.hasRect  = TRUE;
        g_ov.anchor   = img;
        g_ov.cursor   = img;
        SetCapture(g_ov.hwnd);
        break;
    }
    Render();
}

static void OnLeftUp(POINT img)
{
    RECT r;

    Log_Printf(L"overlay: up at %d,%d dragging=%d", img.x, img.y, (int)g_ov.dragging);
    if (!g_ov.dragging)
        return;
    g_ov.dragging = FALSE;
    ReleaseCapture();
    g_ov.cursor = img;

    if (g_ov.mode == SNIP_FREE) {
        if (g_ov.polyCount < 3) {       /* a tap, not a shape */
            PolyReset();
            Render();
            return;
        }
        Finish(TRUE, L"freeform released");
        return;
    }

    r = NormRect(g_ov.anchor, img);
    ClampToImage(&r);
    if (r.right - r.left < 2 || r.bottom - r.top < 2) {
        g_ov.hasRect = FALSE;            /* treat a click as "nothing yet" */
        Render();
        return;
    }
    Finish(TRUE, L"rectangle released");
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p;

    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        if (msg == WM_MOUSEMOVE)        OnMouseMove(p);
        else if (msg == WM_LBUTTONDOWN) OnLeftDown(p);
        else                            OnLeftUp(p);
        return 0;

    case WM_RBUTTONUP:
        /* Right-click backs out of a drag first, and only then out of the
           overlay - the same as Snip & Sketch. */
        if (g_ov.dragging) {
            g_ov.dragging = FALSE;
            g_ov.hasRect  = FALSE;
            ReleaseCapture();
            PolyReset();
            Render();
        } else {
            Finish(FALSE, L"right click");
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT cp;
            GetCursorPos(&cp);
            SetCursor(LoadCursorW(NULL,
                Toolbar_Contains(&g_ov.tb, cp) ? IDC_ARROW : IDC_CROSS));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: Finish(FALSE, L"escape");  return 0;
        case '1':       SetMode(SNIP_RECT);     return 0;
        case '2':       SetMode(SNIP_FREE);     return 0;
        case '3':       SetMode(SNIP_WINDOW);   return 0;
        case '4':       g_ov.mode = SNIP_FULLSCREEN; Finish(TRUE, L"key 4"); return 0;
        case VK_F12:
            if (g_cfg.debugLog)
                DumpBackBuffer();
            return 0;
        default: break;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_ACTIVATE:
        /* The fast path out when activation is handed over politely. The
           first message can arrive before the window has ever been active,
           which is not the case being guarded against. lParam names the
           window that is taking over. */
        if (LOWORD(wp) != WA_INACTIVE)
            g_ov.everActive = TRUE;
        else if (g_ov.everActive && !g_ov.done &&
                 !ReclaimActivation(hwnd, (HWND)lp, L"lost activation"))
            Finish(FALSE, L"lost activation");
        return 0;

    case WM_OV_RECLAIM:
        if (!g_ov.done)
            ForceForeground(hwnd);
        return 0;

    case WM_TIMER:
        /* The slow path, for when it is not: another process can take the
           foreground without this window ever seeing WM_ACTIVATE. */
        if (wp == TIMER_WATCHDOG) {
            HWND fg = GetForegroundWindow();
            if (fg == hwnd)
                g_ov.everActive = TRUE;
            else if (g_ov.everActive && !g_ov.done &&
                     !ReclaimActivation(hwnd, fg, L"lost foreground"))
                Finish(FALSE, L"lost foreground");
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_WATCHDOG);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- entry */

/*
 * Names the window that just took activation, so the log says what happened
 * rather than only that something did. The class is enough to tell the Start
 * menu (Windows.UI.Core.CoreWindow), the taskbar (Shell_TrayWnd) and an
 * ordinary application apart.
 */
static void DescribeWindow(HWND w, wchar_t *out, size_t cch)
{
    wchar_t cls[96] = L"?", exe[MAX_PATH] = L"?";
    DWORD   pid = 0;
    HANDLE  proc;

    if (!w) {
        StringCchCopyW(out, cch, L"(none)");
        return;
    }
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    GetWindowThreadProcessId(w, &pid);
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        DWORD n = ARRAYSIZE(exe);
        if (!QueryFullProcessImageNameW(proc, 0, exe, &n))
            StringCchCopyW(exe, ARRAYSIZE(exe), L"?");
        CloseHandle(proc);
    }
    StringCchPrintfW(out, cch, L"%s in %s", cls, PathFindFileNameW(exe));
}

/*
 * Returns TRUE if the loss was absorbed. Inside the grace window it takes the
 * activation back, a bounded number of times; outside it, or once the budget
 * is spent, it declines and the caller closes the overlay as it always did -
 * that is what stops an overlay being stranded over everything.
 */
static BOOL ReclaimActivation(HWND hwnd, HWND thief, const wchar_t *how)
{
    wchar_t   who[160];
    ULONGLONG age = GetTickCount64() - g_ov.shownAt;

    /* WM_ACTIVATE only names the other window when it belongs to this same
       thread - for another process, which is the case that matters, lParam
       is NULL. By the time the loss is announced the foreground has moved,
       so asking for it directly names the culprit. */
    if (!thief || thief == hwnd)
        thief = GetForegroundWindow();
    if (thief == hwnd)
        thief = NULL;
    DescribeWindow(thief, who, ARRAYSIZE(who));

    if (age < GRACE_MS && g_ov.reclaims < GRACE_RECLAIMS) {
        g_ov.reclaims++;
        Log_Printf(L"overlay: %s to %s after %llu ms - taking it back (%d)",
                   how, who, age, g_ov.reclaims);
        /* Posted, not done here: calling SetForegroundWindow from inside the
           WM_ACTIVATE that announces the loss fights the switch still in
           progress, and the two windows end up trading activation. */
        PostMessageW(hwnd, WM_OV_RECLAIM, 0, 0);
        return TRUE;
    }
    Log_Printf(L"overlay: %s to %s after %llu ms", how, who, age);
    return FALSE;
}

static void ForceForeground(HWND hwnd)
{
    HWND  fg  = GetForegroundWindow();
    DWORD fgT = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD me  = GetCurrentThreadId();

    /* A process that did not produce the last input event is not allowed to
       take the foreground; borrowing the current owner's input queue is the
       documented way round it. */
    if (fgT && fgT != me)
        AttachThreadInput(me, fgT, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (fgT && fgT != me)
        AttachThreadInput(me, fgT, FALSE);
}

static UINT DpiForPoint(POINT screen)
{
    HMONITOR mon = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    HMODULE  shc = LoadLibraryW(L"shcore.dll");
    UINT     x = 96, y = 96;

    if (shc) {
        typedef HRESULT (WINAPI *PFN)(HMONITOR, int, UINT *, UINT *);
        PFN f = (PFN)(void *)GetProcAddress(shc, "GetDpiForMonitor");
        if (f)
            f(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y);
        FreeLibrary(shc);
    }
    return x ? x : 96;
}

static RECT MonitorRectFor(POINT screen)
{
    MONITORINFO mi;
    HMONITOR    mon = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);

    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi))
        return mi.rcMonitor;
    return g_ov.bounds;
}

BOOL Overlay_Run(const SnipImage *frozen, const RECT *bounds, SnipMode start,
                 SnipSelection *sel)
{
    static const wchar_t *cls = L"Nitshot.Overlay";
    WNDCLASSEXW wc;
    MSG         msg;
    POINT       cur;
    RECT        mon;

    if (!frozen || !frozen->pixels || !bounds || !sel)
        return FALSE;
    ZeroMemory(sel, sizeof(*sel));

    /* The previous run's heap blocks are reused; everything else starts clean. */
    {
        POINT *keepPoly = g_ov.poly;
        int    keepCap  = g_ov.polyCap;
        BYTE  *keepMask = g_ov.maskBits;
        SIZE_T keepMCap = g_ov.maskCap;
        ZeroMemory(&g_ov, sizeof(g_ov));
        g_ov.poly     = keepPoly;
        g_ov.polyCap  = keepCap;
        g_ov.maskBits = keepMask;
        g_ov.maskCap  = keepMCap;
    }
    g_ov.frozen = frozen;
    g_ov.bounds = *bounds;
    g_ov.mode   = start;
    g_ov.tb.hot = -1;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_CROSS);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);     /* harmless if it already exists */

    g_ov.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls, L"",
                                WS_POPUP,
                                bounds->left, bounds->top,
                                bounds->right - bounds->left,
                                bounds->bottom - bounds->top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!g_ov.hwnd)
        return FALSE;

    if (!CreateDevice()) {
        Log_Printf(L"overlay: D3D init failed");
        DestroyWindow(g_ov.hwnd);
        DestroyDevice();
        return FALSE;
    }

    GetCursorPos(&cur);
    g_ov.cursor.x = cur.x - bounds->left;
    g_ov.cursor.y = cur.y - bounds->top;

    mon = MonitorRectFor(cur);
    Toolbar_Layout(&g_ov.tb, &mon, DpiForPoint(cur));
    g_ov.tb.active = (start == SNIP_RECT)   ? TB_RECT :
                     (start == SNIP_FREE)   ? TB_FREE :
                     (start == SNIP_WINDOW) ? TB_WINDOW : TB_FULL;
    UploadToolbar();
    if (start == SNIP_WINDOW)
        UpdateHoverWindow(cur);

    /* Draw before showing, so the overlay never flashes an empty frame. */
    Render();
    g_ov.shownAt  = GetTickCount64();
    g_ov.reclaims = 0;
    ShowWindow(g_ov.hwnd, SW_SHOW);
    ForceForeground(g_ov.hwnd);
    SetTimer(g_ov.hwnd, TIMER_WATCHDOG, WATCHDOG_MS, NULL);
    Render();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    sel->taken  = g_ov.taken;
    sel->record = g_ov.recordMode;
    sel->mode   = g_ov.mode;
    if (g_ov.taken) {
        sel->rect = CurrentRect();
        if (g_ov.mode == SNIP_FREE && g_ov.polyCount > 2) {
            SIZE_T bytes = (SIZE_T)g_ov.polyCount * sizeof(POINT);
            sel->poly = (POINT *)HeapAlloc(GetProcessHeap(), 0, bytes);
            if (sel->poly) {
                memcpy(sel->poly, g_ov.poly, bytes);
                sel->polyCount = g_ov.polyCount;
            }
        }
        if (sel->rect.right <= sel->rect.left || sel->rect.bottom <= sel->rect.top)
            sel->taken = FALSE;
    }

    DestroyDevice();
    return TRUE;
}

void Overlay_FreeSelection(SnipSelection *sel)
{
    if (sel && sel->poly) {
        HeapFree(GetProcessHeap(), 0, sel->poly);
        sel->poly = NULL;
        sel->polyCount = 0;
    }
}

BOOL Overlay_ExtractSelection(const SnipImage *frozen, const SnipSelection *sel,
                              SnipImage *out)
{
    if (!frozen || !sel || !out || !sel->taken)
        return FALSE;
    if (!Snip_CropImage(frozen, &sel->rect, out))
        return FALSE;

    if (sel->mode == SNIP_FREE && sel->poly && sel->polyCount > 2) {
        BYTE  *mask;
        UINT32 x, y;

        mask = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)out->width * out->height);
        if (!mask)
            return TRUE;     /* the rectangle is still a usable snip */

        Poly_RasterizeMask(sel->poly, sel->polyCount, &sel->rect,
                           mask, (int)out->width);

        /* Premultiplied: a pixel at 40 % coverage must be 40 % of its colour,
           or pasting onto a light background shows a dark fringe. */
        for (y = 0; y < out->height; y++) {
            const BYTE *m = mask + (SIZE_T)y * out->width;

            if (out->hdr) {
                UINT16 *px = (UINT16 *)(out->pixels + (SIZE_T)y * out->stride);
                for (x = 0; x < out->width; x++) {
                    float a = m[x] / 255.0f;
                    int   i;
                    for (i = 0; i < 3; i++)
                        px[x * 4 + i] =
                            Hdr_FloatToHalf(Hdr_HalfToFloat(px[x * 4 + i]) * a);
                    px[x * 4 + 3] = Hdr_FloatToHalf(a);
                }
            } else {
                BYTE *px = out->pixels + (SIZE_T)y * out->stride;
                for (x = 0; x < out->width; x++) {
                    unsigned a = m[x];
                    px[x * 4 + 0] = (BYTE)(px[x * 4 + 0] * a / 255);
                    px[x * 4 + 1] = (BYTE)(px[x * 4 + 1] * a / 255);
                    px[x * 4 + 2] = (BYTE)(px[x * 4 + 2] * a / 255);
                    px[x * 4 + 3] = (BYTE)a;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, mask);
    }
    return TRUE;
}
