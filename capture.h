/*
 * capture.h - getting pixels off the screen.
 *
 * Two paths, chosen at init and re-checked on every grab:
 *
 *   1. DXGI Desktop Duplication. Fast, GPU-side, and - unlike
 *      Windows.Graphics.Capture - it draws no "you are being captured"
 *      border, which is the whole reason that API is avoided here.
 *   2. Plain GDI BitBlt. The fallback for indirect and virtual displays
 *      (remote desktop hosts, some docking dongles) where duplication is
 *      either unavailable or refuses to hand out frames.
 *
 * Callers never choose; Capture_VirtualDesktop() falls back on its own and
 * Capture_LastMethod() says which path produced the last image.
 */
#ifndef NITSHOT_CAPTURE_H
#define NITSHOT_CAPTURE_H

#include "nitshot.h"

/*
 * A captured image, always top-down.
 *
 *   hdr == FALSE : 32bpp BGRA, 8 bits per channel, sRGB, opaque alpha.
 *   hdr == TRUE  : 64bpp RGBA half-float, scRGB - linear, Rec.709 primaries,
 *                  1.0 == 80 nits. Values above 1.0 are real highlights and
 *                  values below 0.0 are colours outside sRGB; neither is an
 *                  error, and neither survives conversion to 8 bits.
 *
 * Note the channel order differs between the two: that is not an oversight but
 * what Desktop Duplication hands over in each case, and converting one to the
 * other purely for tidiness would cost a pass over 8 megapixels.
 */
typedef struct {
    UINT32 width;
    UINT32 height;
    UINT32 stride;         /* bytes per row                          */
    UINT32 bpp;            /* bytes per pixel: 4 (SDR) or 8 (HDR)    */
    BYTE  *pixels;         /* Snip_FreeImage() releases this         */
    BOOL   hdr;
    float  sdrWhiteNits;   /* HDR only: where SDR white sits         */
} SnipImage;

void Snip_FreeImage(SnipImage *img);

/* Allocates a black, opaque SDR image. FALSE on overflow or out of memory. */
BOOL Snip_AllocImage(SnipImage *img, UINT32 width, UINT32 height);

/* As above, but 'hdr' picks the 64bpp floating-point layout. */
BOOL Snip_AllocImageEx(SnipImage *img, UINT32 width, UINT32 height, BOOL hdr);

/* Crop 'src' to 'rc' (in image coordinates, clipped to the image).
   'dst' gets a fresh allocation. */
BOOL Snip_CropImage(const SnipImage *src, const RECT *rc, SnipImage *dst);

/* Bring the duplication machinery up. Safe to call twice. Returns FALSE only
   if nothing at all works - capture then uses GDI, which never fails. */
BOOL Capture_Init(void);
void Capture_Shutdown(void);

/*
 * Grab every monitor into one image laid out like the virtual desktop.
 * 'bounds' (optional) receives the virtual desktop rectangle in screen
 * coordinates, so image pixel (0,0) is screen point (bounds->left, bounds->top).
 */
BOOL Capture_VirtualDesktop(SnipImage *out, RECT *bounds);

/* "Desktop Duplication", "Desktop Duplication (HDR)", "GDI", or "-". */
const wchar_t *Capture_LastMethod(void);

#endif /* NITSHOT_CAPTURE_H */
