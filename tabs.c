/*
 * tabs.c - the settings dialog's tab strip.
 */

#include "nitshot.h"
#include "tabs.h"
#include "theme.h"

#include <windowsx.h>
#include <strsafe.h>

typedef struct {
    wchar_t label[TABS_MAX][64];
    int     count;
    int     sel;
    int     hot;            /* under the pointer, or -1 */
    BOOL    tracking;       /* TrackMouseEvent armed for WM_MOUSELEAVE */
    HFONT   font;           /* the dialog's, handed over by WM_SETFONT */
    RECT    item[TABS_MAX]; /* laid out at paint time */
} TabState;

static TabState *State(HWND h)
{
    return (TabState *)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static int Scale(HWND h, int px)
{
    UINT dpi = GetDpiForWindow(h);
    return MulDiv(px, dpi ? (int)dpi : 96, 96);
}

/*
 * The selected tab gets a coloured bar under its label - the pivot style
 * Windows' own settings use - rather than a raised tab, which would need a
 * pane beneath it to join, and the point of this control is not having one.
 */
static COLORREF Accent(void)
{
    return Theme_IsDark() ? RGB(77, 194, 255) : RGB(0, 103, 192);
}

static void Layout(HWND h, TabState *s, HDC dc)
{
    RECT  rc;
    int   x, i, pad = Scale(h, 12);
    HFONT old = s->font ? (HFONT)SelectObject(dc, s->font) : NULL;

    GetClientRect(h, &rc);
    x = 0;
    for (i = 0; i < s->count; i++) {
        SIZE sz = { 0, 0 };
        GetTextExtentPoint32W(dc, s->label[i], (int)wcslen(s->label[i]), &sz);
        SetRect(&s->item[i], x, 0, x + sz.cx + 2 * pad, rc.bottom);
        x = s->item[i].right;
    }
    if (old)
        SelectObject(dc, old);
}

static void Paint(HWND h, TabState *s)
{
    PAINTSTRUCT ps;
    HDC         dc = BeginPaint(h, &ps);
    RECT        rc, line;
    HBRUSH      br;
    HFONT       old;
    int         i, bar = Scale(h, 3), rule = Scale(h, 1);

    GetClientRect(h, &rc);
    Layout(h, s, dc);

    br = CreateSolidBrush(Theme_Bg());
    FillRect(dc, &rc, br);
    DeleteObject(br);

    /* A hairline across the whole width separates the strip from the page. */
    SetRect(&line, 0, rc.bottom - rule, rc.right, rc.bottom);
    br = CreateSolidBrush(Theme_Line());
    FillRect(dc, &line, br);
    DeleteObject(br);

    old = s->font ? (HFONT)SelectObject(dc, s->font) : NULL;
    SetBkMode(dc, TRANSPARENT);

    for (i = 0; i < s->count; i++) {
        RECT t = s->item[i];
        BOOL on = i == s->sel;

        SetTextColor(dc, on || i == s->hot ? Theme_Text() : Theme_DimText());
        t.bottom -= bar;
        DrawTextW(dc, s->label[i], -1, &t,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (on) {
            RECT u = s->item[i];
            u.left  += Scale(h, 8);
            u.right -= Scale(h, 8);
            u.top    = u.bottom - bar;
            br = CreateSolidBrush(Accent());
            FillRect(dc, &u, br);
            DeleteObject(br);

            /* Keyboard focus shows as the usual dotted frame, and only when
               Windows says keyboard cues are wanted. */
            if (GetFocus() == h &&
                !(SendMessageW(GetParent(h), WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                RECT f = s->item[i];
                InflateRect(&f, -Scale(h, 3), -Scale(h, 3));
                f.bottom -= bar;
                DrawFocusRect(dc, &f);
            }
        }
    }

    if (old)
        SelectObject(dc, old);
    EndPaint(h, &ps);
}

static int HitTest(TabState *s, int x, int y)
{
    POINT p;
    int   i;

    p.x = x; p.y = y;
    for (i = 0; i < s->count; i++)
        if (PtInRect(&s->item[i], p))
            return i;
    return -1;
}

static void Select(HWND h, TabState *s, int index, BOOL notify)
{
    if (index < 0 || index >= s->count)
        return;
    if (index != s->sel) {
        s->sel = index;
        InvalidateRect(h, NULL, FALSE);
    }
    if (notify)
        SendMessageW(GetParent(h), WM_COMMAND,
                     MAKEWPARAM(GetDlgCtrlID(h), TABN_SELCHANGE), (LPARAM)h);
}

static LRESULT CALLBACK TabsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    TabState *s = State(h);

    switch (msg) {
    case WM_NCCREATE:
        s = (TabState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
        if (!s)
            return FALSE;
        s->hot = -1;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        break;

    case WM_NCDESTROY:
        if (s)
            HeapFree(GetProcessHeap(), 0, s);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;

    case WM_SETFONT:
        if (s) {
            s->font = (HFONT)wp;
            if (LOWORD(lp))
                InvalidateRect(h, NULL, TRUE);
        }
        return 0;

    case WM_GETFONT:
        return s ? (LRESULT)s->font : 0;

    case WM_ERASEBKGND:
        return 1;                       /* Paint fills everything */

    case WM_PAINT:
        if (s)
            Paint(h, s);
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
        InvalidateRect(h, NULL, FALSE);
        break;

    case WM_GETDLGCODE:
        /* The arrows belong to the strip; Tab still moves on through the
           dialog as it should. */
        return DLGC_WANTARROWS;

    case WM_KEYDOWN:
        if (!s) break;
        switch (wp) {
        case VK_LEFT:  Select(h, s, s->sel > 0 ? s->sel - 1 : s->count - 1, TRUE); return 0;
        case VK_RIGHT: Select(h, s, (s->sel + 1) % s->count, TRUE);            return 0;
        case VK_HOME:  Select(h, s, 0, TRUE);                                  return 0;
        case VK_END:   Select(h, s, s->count - 1, TRUE);                       return 0;
        default: break;
        }
        break;

    case WM_LBUTTONDOWN:
        if (s) {
            int i = HitTest(s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetFocus(h);
            if (i >= 0)
                Select(h, s, i, TRUE);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (s) {
            int i = HitTest(s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!s->tracking) {
                TRACKMOUSEEVENT t;
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = h;
                t.dwHoverTime = 0;
                s->tracking = TrackMouseEvent(&t);
            }
            if (i != s->hot) {
                s->hot = i;
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (s) {
            s->tracking = FALSE;
            if (s->hot != -1) {
                s->hot = -1;
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

BOOL Tabs_Register(void)
{
    WNDCLASSW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = TabsProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = TABS_CLASS;
    /* Registering twice is harmless; the second call just fails. */
    return RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void Tabs_SetLabels(HWND tabs, const wchar_t *const *labels, int count)
{
    TabState *s = State(tabs);
    int       i;

    if (!s || !labels)
        return;
    if (count > TABS_MAX)
        count = TABS_MAX;
    for (i = 0; i < count; i++)
        StringCchCopyW(s->label[i], ARRAYSIZE(s->label[i]),
                       labels[i] ? labels[i] : L"");
    s->count = count;
    if (s->sel >= count)
        s->sel = 0;
    InvalidateRect(tabs, NULL, TRUE);
}

int Tabs_GetSel(HWND tabs)
{
    TabState *s = State(tabs);
    return s ? s->sel : 0;
}

void Tabs_SetSel(HWND tabs, int index)
{
    TabState *s = State(tabs);
    if (s)
        Select(tabs, s, index, TRUE);
}
