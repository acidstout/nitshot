/*
 * recordbar.c - the stop bar and the frame around the recorded area.
 */

#include "nitshot.h"
#include "recordbar.h"
#include "record.h"

#include <windowsx.h>
#include <strsafe.h>

#define BAR_CLASS    L"Nitshot.RecordBar"
#define FRAME_CLASS  L"Nitshot.RecordFrame"
#define TIMER_TICK   1
#define FRAME_PX     3

static HWND  g_bar;
static HWND  g_frame;
static HWND  g_owner;
static UINT  g_stopMsg;
static HFONT g_font;
static RECT  g_stopRect;      /* the stop button, in bar client coordinates */
static BOOL  g_stopHot;

static int Scaled(int logical, UINT dpi) { return MulDiv(logical, (int)dpi, 96); }

static UINT DpiFor(HWND hwnd)
{
    HMODULE user = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *PFN)(HWND);
    PFN f = user ? (PFN)(void *)GetProcAddress(user, "GetDpiForWindow") : NULL;
    UINT dpi = f ? f(hwnd) : 96;
    return dpi ? dpi : 96;
}

/* ------------------------------------------------------------- the frame */

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(hwnd, &ps);
        RECT        rc;
        HBRUSH      br = CreateSolidBrush(RGB(0xE0, 0x30, 0x30));
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateFrame(const RECT *region)
{
    RECT   outer = *region;
    HRGN   all, hole;
    int    w, h;

    InflateRect(&outer, FRAME_PX, FRAME_PX);
    w = outer.right - outer.left;
    h = outer.bottom - outer.top;

    g_frame = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        FRAME_CLASS, L"", WS_POPUP,
        outer.left, outer.top, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_frame)
        return;

    /* Only the border is part of the window, so the recorded area itself stays
       clickable and is not covered by anything of ours. */
    all  = CreateRectRgn(0, 0, w, h);
    hole = CreateRectRgn(FRAME_PX, FRAME_PX, w - FRAME_PX, h - FRAME_PX);
    CombineRgn(all, all, hole, RGN_DIFF);
    SetWindowRgn(g_frame, all, TRUE);      /* the window owns 'all' now */
    DeleteObject(hole);

    ShowWindow(g_frame, SW_SHOWNOACTIVATE);
}

/* --------------------------------------------------------------- the bar */

static void PaintBar(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC     dc = BeginPaint(hwnd, &ps);
    RECT    rc, dot;
    UINT    dpi = DpiFor(hwnd);
    HBRUSH  back = CreateSolidBrush(RGB(0x20, 0x20, 0x24));
    HBRUSH  red  = CreateSolidBrush(RGB(0xE0, 0x30, 0x30));
    HBRUSH  hot  = CreateSolidBrush(RGB(0x3A, 0x3A, 0x42));
    HGDIOBJ oldFont;
    wchar_t time[32];
    DWORD   ms = Record_ElapsedMs();

    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, back);

    /* the recording dot */
    dot.left   = Scaled(12, dpi);
    dot.top    = (rc.bottom - Scaled(10, dpi)) / 2;
    dot.right  = dot.left + Scaled(10, dpi);
    dot.bottom = dot.top + Scaled(10, dpi);
    {
        HGDIOBJ ob = SelectObject(dc, red);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
    }

    if (g_stopHot)
        FillRect(dc, &g_stopRect, hot);

    oldFont = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xF0, 0xF2, 0xF6));

    StringCchPrintfW(time, ARRAYSIZE(time), L"%lu:%02lu",
                     (unsigned long)(ms / 60000),
                     (unsigned long)((ms / 1000) % 60));
    {
        RECT t = rc;
        t.left = dot.right + Scaled(8, dpi);
        DrawTextW(dc, time, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, L"Stop", -1, &g_stopRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);

    DeleteObject(back);
    DeleteObject(red);
    DeleteObject(hot);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        PaintBar(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wp == TIMER_TICK)
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        POINT p;
        BOOL  was = g_stopHot;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        g_stopHot = PtInRect(&g_stopRect, p);
        if (g_stopHot != was)
            InvalidateRect(hwnd, NULL, FALSE);
        if (g_stopHot) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_stopHot = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        POINT p;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        if (PtInRect(&g_stopRect, p) && g_owner)
            PostMessageW(g_owner, g_stopMsg, 0, 0);
        return 0;
    }

    case WM_NCHITTEST: {
        /* Anywhere but the button drags the bar out of the way. */
        POINT p;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &p);
        return PtInRect(&g_stopRect, p) ? HTCLIENT : HTCAPTION;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ entry */

static void RegisterClasses(void)
{
    static BOOL done;
    WNDCLASSEXW wc;

    if (done)
        return;
    done = TRUE;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpfnWndProc   = BarProc;
    wc.lpszClassName = BAR_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = FrameProc;
    wc.lpszClassName = FRAME_CLASS;
    RegisterClassExW(&wc);
}

BOOL RecordBar_Show(HWND owner, const RECT *region, UINT stopMsg)
{
    MONITORINFO mi;
    UINT dpi;
    int  w, h, x, y;

    if (!region)
        return FALSE;
    RecordBar_Hide();
    RegisterClasses();

    g_owner   = owner;
    g_stopMsg = stopMsg;
    g_stopHot = FALSE;

    CreateFrame(region);

    dpi = 96;
    {
        HMONITOR mon = MonitorFromRect(region, MONITOR_DEFAULTTONEAREST);
        HMODULE  shc = LoadLibraryW(L"shcore.dll");
        if (shc) {
            typedef HRESULT (WINAPI *PFN)(HMONITOR, int, UINT *, UINT *);
            PFN f = (PFN)(void *)GetProcAddress(shc, "GetDpiForMonitor");
            UINT dx = 96, dy = 96;
            if (f && SUCCEEDED(f(mon, 0, &dx, &dy)) && dx)
                dpi = dx;
            FreeLibrary(shc);
        }
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
            SetRect(&mi.rcWork, region->left, region->top, region->right, region->bottom);
    }

    w = Scaled(160, dpi);
    h = Scaled(40, dpi);
    SetRect(&g_stopRect, w - Scaled(62, dpi), Scaled(6, dpi),
                         w - Scaled(8, dpi),  h - Scaled(6, dpi));

    if (g_font)
        DeleteObject(g_font);
    g_font = CreateFontW(-Scaled(13, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    /* Just below the frame, or just above it when the area reaches the bottom
       of the screen - never on top of what is being recorded. */
    x = (region->left + region->right) / 2 - w / 2;
    y = region->bottom + FRAME_PX + Scaled(8, dpi);
    if (y + h > mi.rcWork.bottom)
        y = region->top - FRAME_PX - Scaled(8, dpi) - h;
    if (y < mi.rcWork.top)
        y = mi.rcWork.top + Scaled(8, dpi);
    if (x < mi.rcWork.left)
        x = mi.rcWork.left;
    if (x + w > mi.rcWork.right)
        x = mi.rcWork.right - w;

    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            BAR_CLASS, L"", WS_POPUP, x, y, w, h,
                            NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_bar) {
        RecordBar_Hide();
        return FALSE;
    }
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                      Scaled(10, dpi), Scaled(10, dpi));
        SetWindowRgn(g_bar, rgn, TRUE);
    }
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    SetTimer(g_bar, TIMER_TICK, 250, NULL);
    return TRUE;
}

void RecordBar_Hide(void)
{
    if (g_bar) {
        KillTimer(g_bar, TIMER_TICK);
        DestroyWindow(g_bar);
        g_bar = NULL;
    }
    if (g_frame) {
        DestroyWindow(g_frame);
        g_frame = NULL;
    }
    if (g_font) {
        DeleteObject(g_font);
        g_font = NULL;
    }
    g_owner = NULL;
}
