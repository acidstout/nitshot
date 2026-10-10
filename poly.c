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

/*
 * One non-horizontal edge, oriented top to bottom. It covers sub-scanlines
 * with y0 <= sy < y1 - half-open, so a vertex shared by two edges is crossed
 * once and not twice - and at sy its x is x0 + (sy - y0) * dxdy.
 */
typedef struct {
    float y0, y1;
    float x0, dxdy;
} Edge;

static int CompareEdgeTop(const void *a, const void *b)
{
    float ya = ((const Edge *)a)->y0, yb = ((const Edge *)b)->y0;
    return (ya > yb) - (ya < yb);
}

/*
 * The crossings on one sub-scanline are few - two for a simple outline, a
 * handful for a self-crossing scribble - so an insertion sort beats qsort's
 * call per comparison by a wide margin.
 */
static void SortCrossings(float *xs, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        float v = xs[i];
        for (j = i; j > 0 && xs[j - 1] > v; j--)
            xs[j] = xs[j - 1];
        xs[j] = v;
    }
}

/*
 * Scanline fill with an active edge table.
 *
 * The freeform preview re-rasterises the whole outline on every new point, so
 * this runs once per mouse move during a drag. Testing every edge against
 * every sub-scanline cost rows x 4 x points - for a large outline, millions of
 * tests per move. Here the edges are sorted by their top once; each
 * sub-scanline then only looks at the edges that actually span it.
 */
void Poly_RasterizeMask(const POINT *pts, int n, const RECT *bbox,
                        BYTE *mask, int stride)
{
    int    w, h, y, i, s;
    int    edgeCount = 0, next = 0, activeCount = 0;
    float *row    = NULL;
    float *xs     = NULL;
    Edge  *edges  = NULL;
    int   *active = NULL;

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

    row    = (float *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * sizeof(float));
    xs     = (float *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(float));
    edges  = (Edge  *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(Edge));
    active = (int   *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(int));
    if (!row || !xs || !edges || !active)
        goto done;

    for (i = 0; i < n; i++) {
        const POINT *a = &pts[i];
        const POINT *b = &pts[i + 1 < n ? i + 1 : 0];
        Edge        *e;

        if (a->y == b->y)
            continue;                 /* horizontal: never crosses a sub-row */
        e = &edges[edgeCount++];
        if (a->y < b->y) {
            e->y0 = (float)a->y;  e->y1 = (float)b->y;
            e->x0 = (float)a->x;
        } else {
            e->y0 = (float)b->y;  e->y1 = (float)a->y;
            e->x0 = (float)b->x;
        }
        e->dxdy = (float)(b->x - a->x) / (float)(b->y - a->y);
    }
    qsort(edges, (size_t)edgeCount, sizeof(Edge), CompareEdgeTop);

    for (y = 0; y < h; y++) {
        BYTE *out = mask + (SIZE_T)y * stride;
        int   x;

        ZeroMemory(row, (SIZE_T)w * sizeof(float));

        for (s = 0; s < SUBSAMPLES; s++) {
            /* Sample through the middle of each sub-row, so a horizontal edge
               lying exactly on a pixel boundary is not counted twice. */
            float sy    = (float)(bbox->top + y) + ((float)s + 0.5f) / SUBSAMPLES;
            int   count = 0, k;

            /* Edges whose top has been reached join the active set... */
            while (next < edgeCount && edges[next].y0 <= sy)
                active[activeCount++] = next++;

            /* ...and the ones that ended above this sub-row leave it. */
            for (k = 0; k < activeCount; ) {
                const Edge *e = &edges[active[k]];
                if (e->y1 <= sy) {
                    active[k] = active[--activeCount];
                    continue;
                }
                xs[count++] = e->x0 + (sy - e->y0) * e->dxdy - (float)bbox->left;
                k++;
            }

            if (count < 2)
                continue;
            SortCrossings(xs, count);
            for (k = 0; k + 1 < count; k += 2)
                AddSpan(row, w, xs[k], xs[k + 1], SUB_WEIGHT);
        }

        for (x = 0; x < w; x++) {
            int v = (int)(row[x] + 0.5f);
            out[x] = (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }

done:
    if (row)    HeapFree(GetProcessHeap(), 0, row);
    if (xs)     HeapFree(GetProcessHeap(), 0, xs);
    if (edges)  HeapFree(GetProcessHeap(), 0, edges);
    if (active) HeapFree(GetProcessHeap(), 0, active);
}
