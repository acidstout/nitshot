/*
 * overlay.h - the full-screen selection UI.
 *
 * Overlay_Run() takes the already-frozen desktop, puts it on screen dimmed,
 * lets the user pick a region, and returns what they chose. It runs its own
 * modal message loop: the host window has no UI of its own, so there is nothing
 * to keep pumping while the overlay is up.
 */
#ifndef NITSHOT_OVERLAY_H
#define NITSHOT_OVERLAY_H

#include "nitshot.h"
#include "capture.h"

typedef struct {
    BOOL     taken;       /* FALSE when the user cancelled                  */
    BOOL     record;      /* the region is to be recorded, not captured     */
    SnipMode mode;        /* the mode it was taken in                       */
    RECT     rect;        /* image coordinates, i.e. relative to 'bounds'   */
    POINT   *poly;        /* freeform outline, image coordinates, or NULL   */
    int      polyCount;
} SnipSelection;

/*
 * 'frozen' is the virtual desktop as captured, 'bounds' its position in screen
 * coordinates. 'start' is the mode the toolbar opens in.
 * Returns FALSE if the overlay could not be brought up at all.
 */
BOOL Overlay_Run(const SnipImage *frozen, const RECT *bounds, SnipMode start,
                 SnipSelection *sel);

void Overlay_FreeSelection(SnipSelection *sel);

/*
 * Cuts 'sel' out of 'frozen' into 'out'. For a freeform selection the pixels
 * outside the outline are left fully transparent, using the same rasteriser
 * that drew the preview.
 */
BOOL Overlay_ExtractSelection(const SnipImage *frozen, const SnipSelection *sel,
                              SnipImage *out);

#endif /* NITSHOT_OVERLAY_H */
