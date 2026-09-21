/*
 * hdr.c - advanced-colour discovery via QueryDisplayConfig.
 */

#include "nitshot.h"
#include "hdr.h"

#include <stdlib.h>
#include <math.h>

#define DEFAULT_SDR_NITS  80.0f

/*
 * The DISPLAYCONFIG_ADVANCED_COLOR_INFO_2 query is Windows 11 only, so this
 * uses the original pair that Windows 10 1709 introduced:
 *   GET_ADVANCED_COLOR_INFO (9)  - is advanced colour on
 *   GET_SDR_WHITE_LEVEL     (11) - where SDR white sits
 */
static BOOL QueryPaths(DISPLAYCONFIG_PATH_INFO **paths, UINT32 *pathCount,
                       DISPLAYCONFIG_MODE_INFO **modes, UINT32 *modeCount)
{
    LONG rc;

    *paths = NULL;
    *modes = NULL;

    /* The buffer can grow between the sizing call and the query, so retry. */
    for (;;) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, pathCount, modeCount)
                != ERROR_SUCCESS)
            return FALSE;

        *paths = (DISPLAYCONFIG_PATH_INFO *)HeapAlloc(GetProcessHeap(), 0,
                     (SIZE_T)*pathCount * sizeof(DISPLAYCONFIG_PATH_INFO));
        *modes = (DISPLAYCONFIG_MODE_INFO *)HeapAlloc(GetProcessHeap(), 0,
                     (SIZE_T)*modeCount * sizeof(DISPLAYCONFIG_MODE_INFO));
        if (!*paths || !*modes) {
            if (*paths) HeapFree(GetProcessHeap(), 0, *paths);
            if (*modes) HeapFree(GetProcessHeap(), 0, *modes);
            *paths = NULL;
            *modes = NULL;
            return FALSE;
        }

        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, pathCount, *paths,
                                modeCount, *modes, NULL);
        if (rc == ERROR_SUCCESS)
            return TRUE;

        HeapFree(GetProcessHeap(), 0, *paths);
        HeapFree(GetProcessHeap(), 0, *modes);
        *paths = NULL;
        *modes = NULL;
        if (rc != ERROR_INSUFFICIENT_BUFFER)
            return FALSE;
    }
}

static void ReadPath(const DISPLAYCONFIG_PATH_INFO *p, HdrInfo *out)
{
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO adv;
    DISPLAYCONFIG_SDR_WHITE_LEVEL         white;

    ZeroMemory(&adv, sizeof(adv));
    adv.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    adv.header.size      = sizeof(adv);
    adv.header.adapterId = p->targetInfo.adapterId;
    adv.header.id        = p->targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&adv.header) == ERROR_SUCCESS) {
        out->supported = adv.advancedColorSupported != 0;
        out->active    = adv.advancedColorEnabled != 0;
    }

    ZeroMemory(&white, sizeof(white));
    white.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size      = sizeof(white);
    white.header.adapterId = p->targetInfo.adapterId;
    white.header.id        = p->targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
        white.SDRWhiteLevel > 0) {
        out->sdrWhiteNits = (float)white.SDRWhiteLevel / 1000.0f * 80.0f;
    }
}

BOOL Hdr_QueryMonitor(const wchar_t *gdiDeviceName, HdrInfo *out)
{
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 pathCount = 0, modeCount = 0, i;
    BOOL   found = FALSE;

    if (!out)
        return FALSE;
    ZeroMemory(out, sizeof(*out));
    out->sdrWhiteNits = DEFAULT_SDR_NITS;

    if (!gdiDeviceName || !gdiDeviceName[0])
        return FALSE;
    if (!QueryPaths(&paths, &pathCount, &modes, &modeCount))
        return FALSE;

    for (i = 0; i < pathCount && !found; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src;

        ZeroMemory(&src, sizeof(src));
        src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size      = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS)
            continue;
        if (_wcsicmp(src.viewGdiDeviceName, gdiDeviceName) != 0)
            continue;

        ReadPath(&paths[i], out);
        found = TRUE;
    }

    HeapFree(GetProcessHeap(), 0, paths);
    HeapFree(GetProcessHeap(), 0, modes);
    return found;
}

BOOL Hdr_AnyDisplayActive(void)
{
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 pathCount = 0, modeCount = 0, i;
    BOOL   any = FALSE;

    if (!QueryPaths(&paths, &pathCount, &modes, &modeCount))
        return FALSE;

    for (i = 0; i < pathCount && !any; i++) {
        HdrInfo info;
        ZeroMemory(&info, sizeof(info));
        info.sdrWhiteNits = DEFAULT_SDR_NITS;
        ReadPath(&paths[i], &info);
        any = info.active;
    }

    HeapFree(GetProcessHeap(), 0, paths);
    HeapFree(GetProcessHeap(), 0, modes);
    return any;
}

/* ------------------------------------------------------------ half floats */

float Hdr_HalfToFloat(UINT16 h)
{
    UINT32 sign = (UINT32)(h >> 15) << 31;
    UINT32 exp  = (h >> 10) & 0x1F;
    UINT32 man  = h & 0x3FF;
    UINT32 bits;
    float  f;

    if (exp == 0) {
        if (man == 0) {
            bits = sign;                       /* +-0 */
        } else {
            /* Subnormal: normalise it by hand. */
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) {
                man <<= 1;
                exp--;
            }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13);   /* inf / NaN */
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }

    memcpy(&f, &bits, sizeof(f));
    return f;
}

UINT16 Hdr_FloatToHalf(float f)
{
    UINT32 bits;
    UINT32 sign, exp, man;
    INT32  e;

    memcpy(&bits, &f, sizeof(bits));
    sign = (bits >> 16) & 0x8000;
    exp  = (bits >> 23) & 0xFF;
    man  = bits & 0x7FFFFF;

    if (exp == 0xFF)                                 /* inf / NaN */
        return (UINT16)(sign | 0x7C00 | (man ? 0x200 : 0));

    e = (INT32)exp - 127 + 15;
    if (e >= 0x1F)
        return (UINT16)(sign | 0x7C00);              /* overflow -> inf */
    if (e <= 0) {
        if (e < -10)
            return (UINT16)sign;                     /* underflow -> 0 */
        man |= 0x800000;
        man >>= (1 - e);
        return (UINT16)(sign | (man >> 13));
    }
    return (UINT16)(sign | ((UINT32)e << 10) | (man >> 13));
}

/* --------------------------------------------------------- tone mapping */

float Hdr_SrgbToLinear(float s)
{
    if (s <= 0.04045f)
        return s / 12.92f;
    return (float)pow((double)((s + 0.055f) / 1.055f), 2.4);
}

static float LinearToSrgb(float l)
{
    if (l <= 0.0031308f)
        return l * 12.92f;
    return 1.055f * (float)pow((double)l, 1.0 / 2.4) - 0.055f;
}

/*
 * Getting HDR into 8 bits forces a choice, because keeping SDR white at 255 and
 * representing anything brighter than white are contradictory demands: once
 * white is 255 there are no code values left above it. So there are two curves
 * and the caller picks.
 *
 * Clipping (the default) keeps SDR content bit-exact - a snip of an ordinary
 * window looks exactly as it did on screen - and everything brighter than white
 * becomes white. That is what someone pasting a screenshot into a chat expects,
 * and the .jxr beside it still holds the real values.
 *
 * The roll-off trades the top of the range for highlight detail: SDR white
 * lands near 233 instead of 255 and the reclaimed headroom separates 1x, 2x and
 * 4x white into distinct values. Worth having for a bright game or a photo,
 * visibly duller for a desktop, which is why it is not the default.
 *
 * Neither touches an SDR capture: those are already 8 bit and never reach the
 * tone mapper at all.
 */
#define KNEE     0.75f     /* identity below this linear value          */
#define HEADROOM 4.0f      /* highlights up to this multiple of white   */

static float RollOff(float x)
{
    /* Shoulder scaled so HEADROOM lands ~95 % of the way to 1.0; the two
       halves meet with matching slope at the knee, so there is no crease. */
    const float span = 1.0f - KNEE;
    const float s    = (HEADROOM - KNEE) / 3.0f;

    if (x <= KNEE)
        return x;
    return KNEE + span * (1.0f - (float)exp(-(double)(x - KNEE) / s));
}

void Hdr_ToneMapScRgbToSrgb(const UINT16 *src, UINT32 srcStridePx,
                            BYTE *dst, UINT32 dstStrideBytes,
                            UINT32 width, UINT32 height, float sdrWhiteNits,
                            BOOL rollOff)
{
    UINT32 x, y, i;
    float  scale;
    BYTE  *lut;

    if (!src || !dst)
        return;
    if (sdrWhiteNits < 1.0f)
        sdrWhiteNits = DEFAULT_SDR_NITS;

    /* scRGB 1.0 is 80 nits by definition, so this is the scRGB value that the
       display is currently calling SDR white. Dividing by it puts white back
       at 1.0 where the sRGB transfer function expects it. */
    scale = 80.0f / sdrWhiteNits;

    /*
     * A half float has only 65536 possible values, so the entire curve is a
     * 64 KB table. Doing it per pixel instead meant three pow() calls each:
     * on a 3840x2160 capture that was 25 million of them and took six seconds.
     */
    lut = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 65536);
    if (!lut)
        return;

    for (i = 0; i < 65536; i++) {
        float v = Hdr_HalfToFloat((UINT16)i) * scale;

        /* Written as a negated comparison so it also catches NaN, which every
           ordinary test would let through into the cast below. */
        if (!(v > 0.0f))
            v = 0.0f;
        if (rollOff)
            v = RollOff(v);
        else if (v > 1.0f)
            v = 1.0f;              /* clip: white stays exactly white */
        v = LinearToSrgb(v);
        if (!(v > 0.0f)) v = 0.0f;
        if (v > 1.0f)    v = 1.0f;
        lut[i] = (BYTE)(v * 255.0f + 0.5f);
    }

    for (y = 0; y < height; y++) {
        const UINT16 *s = src + (SIZE_T)y * srcStridePx;
        BYTE         *d = dst + (SIZE_T)y * dstStrideBytes;

        for (x = 0; x < width; x++) {
            float a = Hdr_HalfToFloat(s[x * 4 + 3]);

            /* BGRA out, RGBA in. */
            if (a >= 1.0f) {
                d[x * 4 + 0] = lut[s[x * 4 + 2]];
                d[x * 4 + 1] = lut[s[x * 4 + 1]];
                d[x * 4 + 2] = lut[s[x * 4 + 0]];
                d[x * 4 + 3] = 0xFF;
            } else if (a <= 0.0f) {
                d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = d[x * 4 + 3] = 0;
            } else {
                /*
                 * A partly covered pixel from a freeform outline. Its colour is
                 * premultiplied, and the transfer function is not linear, so
                 * the coverage has to come off before the curve and go back on
                 * after it - otherwise the edge darkens. Only the outline's own
                 * pixels take this path; everything else uses the table.
                 */
                int ch;
                for (ch = 0; ch < 3; ch++) {
                    float  v   = Hdr_HalfToFloat(s[x * 4 + ch]) / a;
                    UINT16 h16 = Hdr_FloatToHalf(v);
                    d[x * 4 + (2 - ch)] = (BYTE)(lut[h16] * a + 0.5f);
                }
                d[x * 4 + 3] = (BYTE)(a * 255.0f + 0.5f);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, lut);
}
