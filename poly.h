/*
 * poly.h - filling a closed polygon into an 8-bit coverage mask.
 *
 * One rasteriser serves two jobs, which is why it is its own file: the live
 * freeform preview uploads the mask to the GPU every mouse move, and the
 * finished snip uses the very same mask to cut its alpha channel. A second
 * implementation would eventually disagree with the first about an edge pixel.
 */
#ifndef NITSHOT_POLY_H
#define NITSHOT_POLY_H

#include "nitshot.h"

/* The smallest rectangle containing every point, inclusive-exclusive.
   An empty rectangle for n < 1. */
RECT Poly_Bounds(const POINT *pts, int n);

/*
 * Fills the polygon into 'mask', which covers 'bbox' in the same coordinate
 * space as the points: mask[0] is the pixel at (bbox->left, bbox->top).
 * 0 is outside, 255 inside, and edge pixels get partial coverage - the
 * even-odd rule, sampled on four sub-scanlines per row with exact horizontal
 * spans, which is enough anti-aliasing for a hand-drawn outline.
 *
 * The mask is fully overwritten, so it needs no clearing beforehand.
 */
void Poly_RasterizeMask(const POINT *pts, int n, const RECT *bbox,
                        BYTE *mask, int stride);

#endif /* NITSHOT_POLY_H */
