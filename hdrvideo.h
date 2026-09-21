/*
 * hdrvideo.h - scRGB half-float frames to 10-bit BT.2020 PQ, on the GPU.
 *
 * Desktop Duplication hands over an HDR desktop as scRGB: linear, Rec.709
 * primaries, 1.0 = 80 nits. HEVC Main10 wants P010: 10-bit Y'CbCr, 4:2:0,
 * limited range, in the BT.2020 primaries with the SMPTE ST 2084 (PQ) curve.
 * Nothing in Windows does that conversion for us at video rates, so this does
 * it in two draw calls - one full-resolution pass for luma, one half-resolution
 * pass that averages 2x2 chroma - and reads back the two planes.
 *
 * The conversion is absolute, not relative: PQ encodes real luminance, so the
 * SDR white level does not enter into it. That is the whole reason an HDR
 * recording keeps highlights an 8-bit one has already thrown away.
 */
#ifndef NITSHOT_HDRVIDEO_H
#define NITSHOT_HDRVIDEO_H

#include "nitshot.h"

#include <d3d11.h>

typedef struct HdrVideo HdrVideo;

/* Both dimensions must be even - 4:2:0 has no meaning otherwise. */
HdrVideo *HdrVideo_Create(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                          UINT32 width, UINT32 height);

/*
 * Where SDR white sits, so the SDR conversion below can put it back at 1.0,
 * and whether highlights above it roll off or clip. Only used by
 * HdrVideo_ConvertToBgra; the HDR path is absolute and needs neither.
 */
void HdrVideo_SetSdrWhite(HdrVideo *hv, float sdrWhiteNits, BOOL rollOff);
void      HdrVideo_Destroy(HdrVideo *hv);

/* The R16G16B16A16_FLOAT texture to copy each cropped frame into. */
ID3D11Texture2D *HdrVideo_Input(HdrVideo *hv);

/*
 * Converts whatever is currently in the input texture and writes P010 into
 * 'dst': the luma plane, then the interleaved chroma plane. 'dst' must hold
 * width * height * 3 bytes.
 */
BOOL HdrVideo_ConvertToP010(HdrVideo *hv, BYTE *dst);

/*
 * The same captured frame as ordinary 8-bit BGRA, tone-mapped the way the
 * still-image path does it: divided by SDR white so that desktop white lands
 * back on 255, then sRGB encoded.
 *
 * This exists because plain DuplicateOutput is not a usable SDR capture on an
 * HDR display. It hands back the scRGB values encoded to 8-bit *without*
 * dividing by the SDR white level, so on a display whose SDR white is 240 nits
 * everything comes out three times too bright and everything above a third of
 * white clips - measured, bar by bar, against a known ramp.
 *
 * 'dst' holds height rows of 'stride' bytes, 4 bytes per pixel.
 */
BOOL HdrVideo_ConvertToBgra(HdrVideo *hv, BYTE *dst, UINT32 stride);

/*
 * How bright the frame just converted actually was, in nits: its brightest
 * pixel and its average. Both are taken from the luma plane while it is being
 * copied out, so they cost almost nothing - the data is already in cache.
 *
 * This is what MaxCLL and MaxFALL are made of. Writing those into the file is
 * not decoration: a player given HDR with no light-level metadata assumes the
 * worst about how bright the content might be and tone-maps for it, which on a
 * screen recording that never exceeds SDR white shows up as a washed-out,
 * slightly warm picture.
 */
void HdrVideo_LastFrameLight(const HdrVideo *hv, double *peakNits,
                             double *averageNits);

/* A 10-bit limited-range PQ luma code as absolute luminance. */
double HdrVideo_CodeToNits(double code);

#endif /* NITSHOT_HDRVIDEO_H */
