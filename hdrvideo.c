/*
 * hdrvideo.c - the scRGB -> BT.2020 PQ -> P010 conversion pipeline.
 */

#define COBJMACROS

#include "nitshot.h"
#include "hdrvideo.h"
#include "log.h"

#include <d3dcompiler.h>
#include <math.h>

struct HdrVideo {
    ID3D11Device        *dev;
    ID3D11DeviceContext *ctx;
    UINT32               width, height;

    ID3D11Texture2D          *src;      /* scRGB fp16, what duplication fills */
    ID3D11ShaderResourceView *srv;

    ID3D11Texture2D        *texY,  *texUV;    /* render targets  */
    ID3D11RenderTargetView *rtvY,  *rtvUV;
    ID3D11Texture2D        *stgY,  *stgUV;    /* readback        */

    ID3D11VertexShader *vs;
    ID3D11PixelShader  *psY, *psUV, *psSdr;

    /* The SDR path, built only when it is asked for. */
    ID3D11Texture2D        *texSdr, *stgSdr;
    ID3D11RenderTargetView *rtvSdr;
    ID3D11Buffer           *cbSdr;
    float                   sdrScale;
    BOOL                    rollOff;

    UINT16              lastPeak;     /* luma codes, from the last frame */
    double              lastMean;
};

/*
 * Two passes over the same maths. Chroma is averaged after the transfer
 * function rather than before it, which is what 4:2:0 subsampling is defined
 * to do; averaging linear light instead shifts saturated edges.
 */
static const char g_hlsl[] =
"Texture2D<float4> Src : register(t0);\n"
"\n"
"struct VSOut { float4 pos : SV_Position; };\n"
"\n"
"VSOut VSMain(uint id : SV_VertexID)\n"
"{\n"
"    VSOut o;\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"static const float3x3 kRgb709To2020 = {\n"
"    0.627403914, 0.329283028, 0.043313058,\n"
"    0.069097289, 0.919540395, 0.011362316,\n"
"    0.016391439, 0.088013308, 0.895595253\n"
"};\n"
"\n"
"float3 PQ(float3 L)\n"
"{\n"
"    const float m1 = 0.1593017578125;\n"
"    const float m2 = 78.84375;\n"
"    const float c1 = 0.8359375;\n"
"    const float c2 = 18.8515625;\n"
"    const float c3 = 18.6875;\n"
"    float3 Lm = pow(max(L, 0.0), m1);\n"
"    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);\n"
"}\n"
"\n"
"float3 ToYuv(int2 p)\n"
"{\n"
"    float3 rgb = Src.Load(int3(p, 0)).rgb;\n"
"    /* scRGB is linear Rec.709 with 1.0 meaning 80 nits. The matrix runs on\n"
"       signed values on purpose: a negative scRGB component is a colour\n"
"       outside Rec.709 that BT.2020 can hold, and clamping first loses it. */\n"
"    float3 bt2020 = max(mul(kRgb709To2020, rgb * 80.0), 0.0);\n"
"    float3 pq = PQ(min(bt2020, 10000.0) / 10000.0);\n"
"\n"
"    const float Kr = 0.2627;\n"
"    const float Kb = 0.0593;\n"
"    float Y  = Kr * pq.r + (1.0 - Kr - Kb) * pq.g + Kb * pq.b;\n"
"    float Cb = (pq.b - Y) / 1.8814;\n"
"    float Cr = (pq.r - Y) / 1.4746;\n"
"    return float3(Y, Cb, Cr);\n"
"}\n"
"\n"
"/* 10-bit limited range, then shifted into the top of a 16-bit word, which is\n"
"   how P010 stores it. 1023 * 64 = 65472, hence the 65535 divisor. */\n"
"float Code(float v) { return clamp(v, 0.0, 1023.0) * 64.0 / 65535.0; }\n"
"\n"
"float PSLuma(VSOut i) : SV_Target\n"
"{\n"
"    float3 yuv = ToYuv(int2(i.pos.xy));\n"
"    return Code(64.0 + yuv.x * 876.0);\n"
"}\n"
"\n"
"cbuffer Sdr : register(b0)\n"
"{\n"
"    float gSdrScale;   /* 80 / SDR white, so white lands on 1.0 */\n"
"    float gRollOff;    /* non-zero to keep highlight detail     */\n"
"    float2 gPad;\n"
"};\n"
"\n"
"/* The same shoulder as hdr.c: identity to the knee, then an exponential that\n"
"   meets it with matching slope so there is no crease. */\n"
"float RollOff(float x)\n"
"{\n"
"    const float knee = 0.75;\n"
"    const float span = 1.0 - knee;\n"
"    const float s    = (4.0 - knee) / 3.0;\n"
"    return x <= knee ? x : knee + span * (1.0 - exp(-(x - knee) / s));\n"
"}\n"
"\n"
"float LinearToSrgb(float l)\n"
"{\n"
"    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
"}\n"
"\n"
"float4 PSSdr(VSOut i) : SV_Target\n"
"{\n"
"    float3 v = Src.Load(int3(int2(i.pos.xy), 0)).rgb * gSdrScale;\n"
"    int c;\n"
"    v = max(v, 0.0);\n"
"    for (c = 0; c < 3; c++)\n"
"        v[c] = gRollOff != 0.0 ? RollOff(v[c]) : min(v[c], 1.0);\n"
"    for (c = 0; c < 3; c++)\n"
"        v[c] = saturate(LinearToSrgb(v[c]));\n"
"    return float4(v, 1.0);\n"
"}\n"
"\n"
"float2 PSChroma(VSOut i) : SV_Target\n"
"{\n"
"    int2 p = int2(i.pos.xy) * 2;\n"
"    float3 a = ToYuv(p);\n"
"    float3 b = ToYuv(p + int2(1, 0));\n"
"    float3 c = ToYuv(p + int2(0, 1));\n"
"    float3 d = ToYuv(p + int2(1, 1));\n"
"    float2 cbcr = (a.yz + b.yz + c.yz + d.yz) * 0.25;\n"
"    return float2(Code(512.0 + cbcr.x * 896.0),\n"
"                  Code(512.0 + cbcr.y * 896.0));\n"
"}\n";

static BOOL CompileShaders(HdrVideo *hv)
{
    HMODULE     lib;
    pD3DCompile compile;
    ID3DBlob   *vsb = NULL, *psa = NULL, *psb = NULL, *psc = NULL, *err = NULL;
    BOOL        ok = FALSE;
    const UINT  flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    lib = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!lib)
        return FALSE;
    compile = (pD3DCompile)(void *)GetProcAddress(lib, "D3DCompile");
    if (!compile)
        return FALSE;

    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "VSMain", "vs_4_0", flags, 0, &vsb, &err)))   goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSLuma", "ps_4_0", flags, 0, &psa, &err)))   goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSChroma", "ps_4_0", flags, 0, &psb, &err))) goto done;
    if (FAILED(compile(g_hlsl, sizeof(g_hlsl) - 1, NULL, NULL, NULL,
                       "PSSdr", "ps_4_0", flags, 0, &psc, &err)))    goto done;

    if (FAILED(ID3D11Device_CreateVertexShader(hv->dev,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
            NULL, &hv->vs)))  goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psa), ID3D10Blob_GetBufferSize(psa),
            NULL, &hv->psY))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
            NULL, &hv->psUV))) goto done;
    if (FAILED(ID3D11Device_CreatePixelShader(hv->dev,
            ID3D10Blob_GetBufferPointer(psc), ID3D10Blob_GetBufferSize(psc),
            NULL, &hv->psSdr))) goto done;
    ok = TRUE;

done:
    if (err) {
        Log_Printf(L"hdrvideo: shader compile said %hs",
                   (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    if (vsb) ID3D10Blob_Release(vsb);
    if (psa) ID3D10Blob_Release(psa);
    if (psb) ID3D10Blob_Release(psb);
    if (psc) ID3D10Blob_Release(psc);
    return ok;
}

static BOOL MakeTex(HdrVideo *hv, UINT32 w, UINT32 h, DXGI_FORMAT fmt,
                    UINT bind, D3D11_USAGE usage, UINT cpu,
                    ID3D11Texture2D **out)
{
    D3D11_TEXTURE2D_DESC d;

    ZeroMemory(&d, sizeof(d));
    d.Width            = w;
    d.Height           = h;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = fmt;
    d.SampleDesc.Count = 1;
    d.Usage            = usage;
    d.BindFlags        = bind;
    d.CPUAccessFlags   = cpu;

    return SUCCEEDED(ID3D11Device_CreateTexture2D(hv->dev, &d, NULL, out));
}

HdrVideo *HdrVideo_Create(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                          UINT32 width, UINT32 height)
{
    HdrVideo *hv;

    if (!dev || !ctx || (width & 1) || (height & 1) || width < 2 || height < 2)
        return NULL;

    hv = (HdrVideo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*hv));
    if (!hv)
        return NULL;
    hv->dev    = dev;
    hv->ctx    = ctx;
    hv->width  = width;
    hv->height = height;

    if (!MakeTex(hv, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                 D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &hv->src))
        goto fail;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)hv->src, NULL, &hv->srv)))
        goto fail;

    if (!MakeTex(hv, width, height, DXGI_FORMAT_R16_UNORM,
                 D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &hv->texY))
        goto fail;
    if (!MakeTex(hv, width / 2, height / 2, DXGI_FORMAT_R16G16_UNORM,
                 D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &hv->texUV))
        goto fail;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev,
            (ID3D11Resource *)hv->texY, NULL, &hv->rtvY)))
        goto fail;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev,
            (ID3D11Resource *)hv->texUV, NULL, &hv->rtvUV)))
        goto fail;

    if (!MakeTex(hv, width, height, DXGI_FORMAT_R16_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &hv->stgY))
        goto fail;
    if (!MakeTex(hv, width / 2, height / 2, DXGI_FORMAT_R16G16_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &hv->stgUV))
        goto fail;

    if (!CompileShaders(hv))
        goto fail;

    return hv;

fail:
    HdrVideo_Destroy(hv);
    return NULL;
}

void HdrVideo_Destroy(HdrVideo *hv)
{
    if (!hv)
        return;
    if (hv->cbSdr)  ID3D11Buffer_Release(hv->cbSdr);
    if (hv->stgSdr) ID3D11Texture2D_Release(hv->stgSdr);
    if (hv->rtvSdr) ID3D11RenderTargetView_Release(hv->rtvSdr);
    if (hv->texSdr) ID3D11Texture2D_Release(hv->texSdr);
    if (hv->psSdr) ID3D11PixelShader_Release(hv->psSdr);
    if (hv->psUV) ID3D11PixelShader_Release(hv->psUV);
    if (hv->psY)  ID3D11PixelShader_Release(hv->psY);
    if (hv->vs)   ID3D11VertexShader_Release(hv->vs);
    if (hv->stgUV) ID3D11Texture2D_Release(hv->stgUV);
    if (hv->stgY)  ID3D11Texture2D_Release(hv->stgY);
    if (hv->rtvUV) ID3D11RenderTargetView_Release(hv->rtvUV);
    if (hv->rtvY)  ID3D11RenderTargetView_Release(hv->rtvY);
    if (hv->texUV) ID3D11Texture2D_Release(hv->texUV);
    if (hv->texY)  ID3D11Texture2D_Release(hv->texY);
    if (hv->srv)   ID3D11ShaderResourceView_Release(hv->srv);
    if (hv->src)   ID3D11Texture2D_Release(hv->src);
    HeapFree(GetProcessHeap(), 0, hv);
}

ID3D11Texture2D *HdrVideo_Input(HdrVideo *hv)
{
    return hv ? hv->src : NULL;
}

/* Renders one full-screen pass; defined below, used by both paths. */
static void Pass(HdrVideo *hv, ID3D11RenderTargetView *rtv,
                 ID3D11PixelShader *ps, UINT32 w, UINT32 h);

void HdrVideo_SetSdrWhite(HdrVideo *hv, float sdrWhiteNits, BOOL rollOff)
{
    if (!hv)
        return;
    if (sdrWhiteNits < 1.0f)
        sdrWhiteNits = 80.0f;
    /* scRGB 1.0 is 80 nits by definition, so this is what puts the level the
       display currently calls white back at 1.0, where sRGB expects it. */
    hv->sdrScale = 80.0f / sdrWhiteNits;
    hv->rollOff  = rollOff;
}

/* Built on first use: an HDR recording never needs any of it. */
static BOOL EnsureSdr(HdrVideo *hv)
{
    D3D11_BUFFER_DESC bd;

    if (hv->texSdr)
        return TRUE;

    if (!MakeTex(hv, hv->width, hv->height, DXGI_FORMAT_B8G8R8A8_UNORM,
                 D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &hv->texSdr))
        return FALSE;
    if (FAILED(ID3D11Device_CreateRenderTargetView(hv->dev,
            (ID3D11Resource *)hv->texSdr, NULL, &hv->rtvSdr)))
        return FALSE;
    if (!MakeTex(hv, hv->width, hv->height, DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &hv->stgSdr))
        return FALSE;

    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth      = 16;          /* one float4, the smallest cbuffer */
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(ID3D11Device_CreateBuffer(hv->dev, &bd, NULL, &hv->cbSdr));
}

BOOL HdrVideo_ConvertToBgra(HdrVideo *hv, BYTE *dst, UINT32 stride)
{
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11ShaderResourceView *none[1] = { NULL };
    UINT32 y;

    if (!hv || !dst || !EnsureSdr(hv))
        return FALSE;
    if (hv->sdrScale <= 0.0f)
        hv->sdrScale = 1.0f;

    if (SUCCEEDED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->cbSdr, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        float *p = (float *)map.pData;
        p[0] = hv->sdrScale;
        p[1] = hv->rollOff ? 1.0f : 0.0f;
        p[2] = p[3] = 0.0f;
        ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->cbSdr, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(hv->ctx, 0, 1, &hv->cbSdr);

    Pass(hv, hv->rtvSdr, hv->psSdr, hv->width, hv->height);

    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->stgSdr,
                                     (ID3D11Resource *)hv->texSdr);

    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->stgSdr, 0,
                                       D3D11_MAP_READ, 0, &map)))
        return FALSE;
    for (y = 0; y < hv->height; y++)
        memcpy(dst + (SIZE_T)y * stride,
               (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
               (SIZE_T)hv->width * 4);
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->stgSdr, 0);
    return TRUE;
}

/* The inverse of the shader's PQ, on a 10-bit limited-range luma code. */
double HdrVideo_CodeToNits(double code)
{
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    double e = (code - 64.0) / 876.0;
    double p, num, den;

    if (e <= 0.0)
        return 0.0;
    if (e > 1.0)
        e = 1.0;
    p   = pow(e, 1.0 / m2);
    num = p - c1;
    den = c2 - c3 * p;
    if (num <= 0.0 || den <= 0.0)
        return 0.0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

void HdrVideo_LastFrameLight(const HdrVideo *hv, double *peakNits,
                             double *averageNits)
{
    /* The codes live in the top ten bits of a sixteen-bit word. */
    if (peakNits)
        *peakNits = hv ? HdrVideo_CodeToNits(hv->lastPeak / 64.0) : 0.0;
    if (averageNits)
        *averageNits = hv ? HdrVideo_CodeToNits(hv->lastMean / 64.0) : 0.0;
}

static void Pass(HdrVideo *hv, ID3D11RenderTargetView *rtv,
                 ID3D11PixelShader *ps, UINT32 w, UINT32 h)
{
    D3D11_VIEWPORT vp;
    ID3D11RenderTargetView *rtvs[1];

    ZeroMemory(&vp, sizeof(vp));
    vp.Width    = (FLOAT)w;
    vp.Height   = (FLOAT)h;
    vp.MaxDepth = 1.0f;

    rtvs[0] = rtv;
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 1, rtvs, NULL);
    ID3D11DeviceContext_RSSetViewports(hv->ctx, 1, &vp);
    ID3D11DeviceContext_VSSetShader(hv->ctx, hv->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(hv->ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, &hv->srv);
    ID3D11DeviceContext_IASetPrimitiveTopology(hv->ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(hv->ctx, NULL);
    ID3D11DeviceContext_Draw(hv->ctx, 3, 0);
}

BOOL HdrVideo_ConvertToP010(HdrVideo *hv, BYTE *dst)
{
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11ShaderResourceView *none[1] = { NULL };
    UINT32 y;
    BYTE  *uvPlane;

    if (!hv || !dst)
        return FALSE;

    Pass(hv, hv->rtvY,  hv->psY,  hv->width,     hv->height);
    Pass(hv, hv->rtvUV, hv->psUV, hv->width / 2, hv->height / 2);

    /* The source is bound for reading and is about to be written again by the
       next frame's copy; leaving it bound makes the debug layer complain and
       the driver insert a barrier of its own. */
    ID3D11DeviceContext_PSSetShaderResources(hv->ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(hv->ctx, 0, NULL, NULL);

    ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->stgY,
                                     (ID3D11Resource *)hv->texY);
    ID3D11DeviceContext_CopyResource(hv->ctx, (ID3D11Resource *)hv->stgUV,
                                     (ID3D11Resource *)hv->texUV);

    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->stgY, 0,
                                       D3D11_MAP_READ, 0, &map)))
        return FALSE;
    {
        UINT16 peak = 0;
        UINT64 sum  = 0;

        for (y = 0; y < hv->height; y++) {
            UINT16 *row = (UINT16 *)(dst + (SIZE_T)y * hv->width * 2);
            UINT32  x;

            memcpy(row, (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
                   (SIZE_T)hv->width * 2);
            /* The row is hot in cache from the copy, so measuring it here is
               far cheaper than a second pass later would be. */
            for (x = 0; x < hv->width; x++) {
                if (row[x] > peak) peak = row[x];
                sum += row[x];
            }
        }
        hv->lastPeak = peak;
        hv->lastMean = (double)sum / ((double)hv->width * hv->height);
    }
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->stgY, 0);

    uvPlane = dst + (SIZE_T)hv->width * hv->height * 2;
    if (FAILED(ID3D11DeviceContext_Map(hv->ctx, (ID3D11Resource *)hv->stgUV, 0,
                                       D3D11_MAP_READ, 0, &map)))
        return FALSE;
    for (y = 0; y < hv->height / 2; y++)
        memcpy(uvPlane + (SIZE_T)y * hv->width * 2,
               (const BYTE *)map.pData + (SIZE_T)y * map.RowPitch,
               (SIZE_T)hv->width * 2);
    ID3D11DeviceContext_Unmap(hv->ctx, (ID3D11Resource *)hv->stgUV, 0);

    return TRUE;
}
