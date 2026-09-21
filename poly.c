/*
 * poly.c - even-odd scanline polygon fill with 4x vertical supersampling.
 */

#include "nitshot.h"
#include "poly.h"

#include <stdlib.h>

#define SUBSAMPLES 4                    /* sub-scanlines per pixel row */
#define SUB_WEIGHT (255.0f / SUBSAMPLES)

RECT Poly_Bounds(const POINT *pts, int n)
{
    RECT r;
    int  i;

    if (!pts || n < 1) {
        SetRectEmpty(&r);
        return r;
    }
    r.left = r.right = pts[0].x;
    r.top  = r.bottom = pts[0].y;
    for (i = 1; i < n; i++) {
        if (pts[i].x < r.left)   r.left   = pts[i].x;
        if (pts[i].x > r.right)  r.right  = pts[i].x;
        if (pts[i].y < r.top)    r.top    = pts[i].y;
        if (pts[i].y > r.bottom) r.bottom = pts[i].y;
    }
    r.right++;      /* the bounding box is inclusive-exclusive */
    r.bottom++;
    return r;
}

static int CompareFloat(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/* Add 'weight' of coverage to [x0, x1) of one row, with fractional ends. */
static void AddSpan(float *row, int width, float x0, float x1, float weight)
{
    int ix0, ix1, x;

    if (x1 <= 0.0f || x0 >= (float)width || x1 <= x0)
        return;
    if (x0 < 0.0f)            x0 = 0.0f;
    if (x1 > (float)width)    x1 = (float)width;

    ix0 = (int)x0;
    ix1 = (int)x1;

    if (ix0 == ix1) {
        row[ix0] += (x1 - x0) * weight;     /* span inside one pixel */
        return;
    }
    row[ix0] += ((float)(ix0 + 1) - x0) * weight;
    for (x = ix0 + 1; x < ix1; x++)
        row[x] += weight;
    if (ix1 < width)
        row[ix1] += (x1 - (float)ix1) * weight;
}

void Poly_RasterizeMask(const POINT *pts, int n, const RECT *bbox,
                        BYTE *mask, int stride)
{
    int    w, h, y, i, s;
    float *row  = NULL;
    float *xs   = NULL;

    if (!mask || !bbox)
        return;
    w = bbox->right  - bbox->left;
    h = bbox->bottom - bbox->top;
    if (w <= 0 || h <= 0)
        return;

    /* Fewer than three points cannot enclose anything. */
    if (!pts || n < 3) {
        for (y = 0; y < h; y++)
            ZeroMemory(mask + (SIZE_T)y * stride, (SIZE_T)w);
        return;
    }

    row = (float *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * sizeof(float));
    xs  = (float *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(float));
    if (!row || !xs) {
        if (row) HeapFree(GetProcessHeap(), 0, row);
        if (xs)  HeapFree(GetProcessHeap(), 0, xs);
        return;
    }

    for (y = 0; y < h; y++) {
        BYTE *out = mask + (SIZE_T)y * stride;
        int   x;

        ZeroMemory(row, (SIZE_T)w * sizeof(float));

        for (s = 0; s < SUBSAMPLES; s++) {
            /* Sample through the middle of each sub-row, so a horizontal edge
               lying exactly on a pixel boundary is not counted twice. */
            float sy    = (float)(bbox->top + y) + ((float)s + 0.5f) / SUBSAMPLES;
            int   count = 0;

            for (i = 0; i < n; i++) {
                const POINT *a = &pts[i];
                const POINT *b = &pts[(i + 1) % n];
                float ay = (float)a->y, by = (float)b->y;

                /* Half-open in y: counts the lower vertex, not the upper one,
                   which is what keeps shared vertices from double-crossing. */
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    float t = (sy - ay) / (by - ay);
                    xs[count++] = (float)a->x + t * (float)(b->x - a->x)
                                  - (float)bbox->left;
                }
            }
            if (count < 2)
                continue;
            qsort(xs, (size_t)count, sizeof(float), CompareFloat);
            for (i = 0; i + 1 < count; i += 2)
                AddSpan(row, w, xs[i], xs[i + 1], SUB_WEIGHT);
        }

        for (x = 0; x < w; x++) {
            int v = (int)(row[x] + 0.5f);
            out[x] = (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }

    HeapFree(GetProcessHeap(), 0, row);
    HeapFree(GetProcessHeap(), 0, xs);
}
