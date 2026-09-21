/*
 * hdr.h - what the display is actually doing, and the arithmetic that follows.
 *
 * Windows composites an advanced-colour display in scRGB: linear, Rec.709
 * primaries, where 1.0 is 80 nits by definition. SDR white does not sit at 1.0
 * but wherever the user's "SDR content brightness" slider put it, which is why
 * the white level has to be read rather than assumed - tone mapping a capture
 * without it turns the whole desktop grey or blows it out.
 */
#ifndef NITSHOT_HDR_H
#define NITSHOT_HDR_H

#include "nitshot.h"

typedef struct {
    BOOL  supported;        /* the display could do advanced colour        */
    BOOL  active;           /* ...and it is on right now                   */
    float sdrWhiteNits;     /* where SDR white sits, in nits (80 if unknown) */
} HdrInfo;

/*
 * Look up one monitor by its GDI device name ("\\\\.\\DISPLAY1", as
 * DXGI_OUTPUT_DESC.DeviceName gives it). Returns FALSE if the display could
 * not be found, in which case 'out' is filled with the SDR defaults.
 */
BOOL Hdr_QueryMonitor(const wchar_t *gdiDeviceName, HdrInfo *out);

/* TRUE if any attached display is in advanced colour right now. */
BOOL Hdr_AnyDisplayActive(void);

/* ------------------------------------------------------- half floats */

float  Hdr_HalfToFloat(UINT16 h);
UINT16 Hdr_FloatToHalf(float f);

/* ---------------------------------------------------- tone mapping */

/*
 * scRGB -> 8-bit sRGB. 'sdrWhiteNits' says which scRGB value is SDR white.
 *
 * 'rollOff' FALSE clips anything above white, keeping SDR content bit-exact;
 * TRUE trades the top of the range for highlight detail. Neither is free - see
 * the curve in hdr.c for why the choice cannot be avoided.
 */
void Hdr_ToneMapScRgbToSrgb(const UINT16 *src, UINT32 srcStridePx,
                            BYTE *dst, UINT32 dstStrideBytes,
                            UINT32 width, UINT32 height, float sdrWhiteNits,
                            BOOL rollOff);

/* sRGB encoded 0..1 -> linear, for putting UI colours into an scRGB target. */
float Hdr_SrgbToLinear(float s);

#endif /* NITSHOT_HDR_H */
