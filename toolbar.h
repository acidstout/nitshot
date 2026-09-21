/*
 * toolbar.h - the floating mode picker at the top of the overlay.
 *
 * The toolbar is rendered on the CPU into a premultiplied BGRA buffer and then
 * uploaded as a texture, rather than drawn with the rest of the overlay. GDI
 * has no anti-aliasing, so each layer is drawn white-on-black at four times the
 * final size and box-filtered down; that is what gives the icons clean edges
 * without dragging Direct2D (which the Windows SDK does not expose to C) or a
 * font into the program.
 */
#ifndef NITSHOT_TOOLBAR_H
#define NITSHOT_TOOLBAR_H

#include "nitshot.h"

typedef enum {
    TB_RECT = 0,
    TB_FREE,
    TB_WINDOW,
    TB_FULL,
    TB_RECORD,
    TB_CLOSE,
    TB_COUNT
} TbButton;

typedef struct {
    RECT rect;                  /* the whole pill, in virtual-desktop pixels */
    RECT button[TB_COUNT];      /* hit rectangles, same space               */
    int  hot;                   /* hovered button, or -1                    */
    int  active;                /* the button for the current mode          */
    UINT dpi;
} Toolbar;

/* Centres the toolbar near the top of 'monitor' (virtual-desktop pixels) and
   sizes it for that monitor's DPI. */
void Toolbar_Layout(Toolbar *tb, const RECT *monitor, UINT dpi);

/* Which button is under 'pt' (virtual-desktop pixels), or -1. */
int Toolbar_HitTest(const Toolbar *tb, POINT pt);

/* TRUE if the point is anywhere on the pill, where the selection must not
   start. */
BOOL Toolbar_Contains(const Toolbar *tb, POINT pt);

/*
 * Renders into a caller-allocated premultiplied BGRA buffer that is
 * (tb->rect width x height) pixels, 'stride' bytes per row.
 * Cheap enough to call whenever the hot or active button changes.
 */
BOOL Toolbar_Render(const Toolbar *tb, BYTE *bgra, int stride);

#endif /* NITSHOT_TOOLBAR_H */
