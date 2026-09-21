/*
 * theme.c - dark mode for the classic Win32 UI.
 *
 * Derived from the SystemInfo project's theme module; the tab control and
 * status bar painters were dropped (this app has neither) and check box,
 * group box and edit support added.
 */

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>

#include "theme.h"

/* ============================================================
 *  Palette
 * ============================================================ */

#define D_BG        RGB(0x2B, 0x2B, 0x2B)   /* window / dialog background */
#define D_SURFACE   RGB(0x20, 0x20, 0x20)   /* list and edit body         */
#define D_FACE      RGB(0x33, 0x33, 0x33)   /* button face                */
#define D_HOT       RGB(0x45, 0x45, 0x45)
#define D_PRESSED   RGB(0x1C, 0x1C, 0x1C)
#define D_LINE      RGB(0x55, 0x55, 0x55)
#define D_TEXT      RGB(0xF0, 0xF0, 0xF0)
#define D_DIM       RGB(0x9A, 0x9A, 0x9A)
#define D_DISABLED  RGB(0x70, 0x70, 0x70)
#define D_ACCENT    RGB(0x6C, 0xA0, 0xE8)
#define D_WARN      RGB(0xFF, 0xBE, 0x3C)
#define D_OK        RGB(0x5C, 0xC8, 0x8A)
#define D_ROWHI     RGB(0x4A, 0x3D, 0x14)

#define L_WARN      RGB(0xB4, 0x64, 0x00)
#define L_OK        RGB(0x00, 0x6E, 0x3C)
#define L_ROWHI     RGB(0xFF, 0xF5, 0xCD)

/* ============================================================
 *  Private uxtheme entry points (ordinals, Win10 1809+)
 * ============================================================ */

typedef enum {
    APPMODE_DEFAULT     = 0,
    APPMODE_ALLOWDARK   = 1,
    APPMODE_FORCEDARK   = 2,
    APPMODE_FORCELIGHT  = 3
} PreferredAppMode;

typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode);
typedef BOOL (WINAPI *fnAllowDarkModeForApp)(BOOL);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *fnRefreshImmersiveColorPolicyState)(void);
typedef BOOL (WINAPI *fnRefreshTitleBarThemeColor)(HWND);
typedef void (WINAPI *fnFlushMenuThemes)(void);
typedef HRESULT (WINAPI *fnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef LONG (WINAPI *fnRtlGetVersion)(RTL_OSVERSIONINFOW *);

static fnSetPreferredAppMode              p_SetPreferredAppMode;
static fnAllowDarkModeForApp              p_AllowDarkModeForApp;
static fnAllowDarkModeForWindow           p_AllowDarkModeForWindow;
static fnRefreshImmersiveColorPolicyState p_RefreshImmersiveColorPolicyState;
static fnRefreshTitleBarThemeColor        p_RefreshTitleBarThemeColor;
static fnFlushMenuThemes                  p_FlushMenuThemes;
static fnDwmSetWindowAttribute            p_DwmSetWindowAttribute;

static BOOL   g_supported = FALSE;
static BOOL   g_dark      = FALSE;
static BOOL   g_inited    = FALSE;
static HBRUSH g_brBg      = NULL;
static HBRUSH g_brSurface = NULL;

/* Subclass ids */
#define SC_LIST    2
#define SC_BUTTON  4
#define SC_GROUP   5
#define SC_CHECK   6

static LRESULT CALLBACK list_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK button_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK group_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK check_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

/* ============================================================
 *  Init / mode
 * ============================================================ */

static DWORD os_build(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    fnRtlGetVersion get;
    RTL_OSVERSIONINFOW vi;
    if (!nt) return 0;
    get = (fnRtlGetVersion)(void *)GetProcAddress(nt, "RtlGetVersion");
    if (!get) return 0;
    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (get(&vi) != 0) return 0;
    if (vi.dwMajorVersion < 10) return 0;
    return vi.dwBuildNumber;
}

BOOL Theme_Init(void)
{
    HMODULE ux, dwm;
    DWORD build;

    if (g_inited) return g_supported;
    g_inited = TRUE;

    build = os_build();
    if (build < 17763)              /* dark mode arrived in 1809 */
        return FALSE;

    ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return FALSE;

    p_RefreshImmersiveColorPolicyState =
        (fnRefreshImmersiveColorPolicyState)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    p_RefreshTitleBarThemeColor =
        (fnRefreshTitleBarThemeColor)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(106));
    p_AllowDarkModeForWindow =
        (fnAllowDarkModeForWindow)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    p_FlushMenuThemes =
        (fnFlushMenuThemes)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));

    /* Ordinal 135 is AllowDarkModeForApp(BOOL) on 1809 and
       SetPreferredAppMode(PreferredAppMode) from 1903 on. */
    if (build < 18362)
        p_AllowDarkModeForApp = (fnAllowDarkModeForApp)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    else
        p_SetPreferredAppMode = (fnSetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));

    dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwm)
        p_DwmSetWindowAttribute =
            (fnDwmSetWindowAttribute)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");

    g_supported = (p_AllowDarkModeForWindow != NULL) &&
                  (p_SetPreferredAppMode != NULL || p_AllowDarkModeForApp != NULL);
    return g_supported;
}

BOOL Theme_Supported(void) { return g_supported; }
BOOL Theme_IsDark(void)    { return g_dark; }

BOOL Theme_SystemPrefersDark(void)
{
    HKEY k;
    DWORD val = 1, cb = sizeof(val), type = REG_DWORD;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, &type,
                         (LPBYTE)&val, &cb) != ERROR_SUCCESS)
        val = 1;
    RegCloseKey(k);
    return val == 0;
}

void Theme_SetDark(BOOL dark)
{
    if (!g_supported) dark = FALSE;
    g_dark = dark;

    if (g_supported) {
        if (p_SetPreferredAppMode)
            /* ForceLight rather than Default: Default follows Windows and
               would override the choice made inside this app. */
            p_SetPreferredAppMode(dark ? APPMODE_FORCEDARK : APPMODE_FORCELIGHT);
        else if (p_AllowDarkModeForApp)
            p_AllowDarkModeForApp(dark);
        if (p_RefreshImmersiveColorPolicyState)
            p_RefreshImmersiveColorPolicyState();
        if (p_FlushMenuThemes)      /* popup menus cache their theme */
            p_FlushMenuThemes();
    }

    if (g_brBg)      { DeleteObject(g_brBg);      g_brBg = NULL; }
    if (g_brSurface) { DeleteObject(g_brSurface); g_brSurface = NULL; }
    g_brBg      = CreateSolidBrush(Theme_Bg());
    g_brSurface = CreateSolidBrush(Theme_Surface());
}

COLORREF Theme_Bg(void)      { return g_dark ? D_BG      : GetSysColor(COLOR_BTNFACE);   }
COLORREF Theme_Surface(void) { return g_dark ? D_SURFACE : GetSysColor(COLOR_WINDOW);    }
COLORREF Theme_Text(void)    { return g_dark ? D_TEXT    : GetSysColor(COLOR_WINDOWTEXT);}
COLORREF Theme_DimText(void) { return g_dark ? D_DIM     : GetSysColor(COLOR_GRAYTEXT);  }
COLORREF Theme_Line(void)    { return g_dark ? D_LINE    : GetSysColor(COLOR_BTNSHADOW); }
COLORREF Theme_Warn(void)    { return g_dark ? D_WARN    : L_WARN;  }
COLORREF Theme_Ok(void)      { return g_dark ? D_OK      : L_OK;    }
COLORREF Theme_RowHighlight(void) { return g_dark ? D_ROWHI : L_ROWHI; }

HBRUSH Theme_BgBrush(void)
{
    if (!g_brBg) g_brBg = CreateSolidBrush(Theme_Bg());
    return g_brBg;
}

HBRUSH Theme_SurfaceBrush(void)
{
    if (!g_brSurface) g_brSurface = CreateSolidBrush(Theme_Surface());
    return g_brSurface;
}

BOOL Theme_IsColorSchemeChange(UINT msg, LPARAM lp)
{
    if (msg != WM_SETTINGCHANGE || !lp) return FALSE;
    return lstrcmpiW((const wchar_t *)lp, L"ImmersiveColorSet") == 0;
}

/* ============================================================
 *  Window application
 * ============================================================ */

void Theme_ApplyToMainWindow(HWND hwnd)
{
    BOOL on = g_dark;

    if (g_supported) {
        if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, on);
        if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();

        /* Win11 path. On Win10 this returns S_OK and does nothing. */
        if (p_DwmSetWindowAttribute) {
            BOOL v = on;
            if (FAILED(p_DwmSetWindowAttribute(hwnd, 20, &v, sizeof(v))))
                p_DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
        }
        /* Win10 path: the caption only repaints on the next activation
           change, so fake one or it shows the previous mode. */
        if (p_RefreshTitleBarThemeColor) p_RefreshTitleBarThemeColor(hwnd);
        if (IsWindowVisible(hwnd)) {
            BOOL act = (GetActiveWindow() == hwnd);
            SendMessageW(hwnd, WM_NCACTIVATE, (WPARAM)!act, 0);
            SendMessageW(hwnd, WM_NCACTIVATE, (WPARAM)act, 0);
        }
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToListView(HWND hwnd)
{
    HWND hdr;
    DWORD ex;
    LONG_PTR style;

    if (!hwnd) return;
    hdr = ListView_GetHeader(hwnd);

    if (g_supported && p_AllowDarkModeForWindow) {
        p_AllowDarkModeForWindow(hwnd, g_dark);
        if (hdr) p_AllowDarkModeForWindow(hdr, g_dark);
    }
    SetWindowTheme(hwnd, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    if (hdr) SetWindowTheme(hdr, g_dark ? L"DarkMode_ItemsView" : NULL, NULL);

    ListView_SetBkColor(hwnd,     Theme_Surface());
    ListView_SetTextBkColor(hwnd, Theme_Surface());
    ListView_SetTextColor(hwnd,   Theme_Text());

    /* Grid lines are painted in a fixed light colour, so drop them. */
    ex = ListView_GetExtendedListViewStyle(hwnd);
    if (g_dark) ex &= ~LVS_EX_GRIDLINES;
    else        ex |=  LVS_EX_GRIDLINES;
    ListView_SetExtendedListViewStyle(hwnd, ex);

    /* WS_BORDER draws a light 3D frame that no theme touches. */
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (g_dark) style &= ~(LONG_PTR)WS_BORDER;
    else        style |=  (LONG_PTR)WS_BORDER;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    SetWindowSubclass(hwnd, list_proc, SC_LIST, 0);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToButton(HWND hwnd)
{
    if (!hwnd) return;
    if (g_supported && p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, g_dark);
    /* No theme suits a push button in dark mode (DarkMode_CFD renders it
       light), so button_proc paints it by hand. */
    SetWindowTheme(hwnd, NULL, NULL);
    SetWindowSubclass(hwnd, button_proc, SC_BUTTON, FALSE);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToCheckBox(HWND hwnd)
{
    if (!hwnd) return;
    if (g_supported && p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, g_dark);
    /* A themed check box draws its own label through uxtheme and ignores the
       colour the parent hands back from WM_CTLCOLORSTATIC, so the text stays
       black on the dark background. check_proc draws box and label instead. */
    SetWindowTheme(hwnd, NULL, NULL);
    SetWindowSubclass(hwnd, check_proc, SC_CHECK, FALSE);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToGroupBox(HWND hwnd)
{
    if (!hwnd) return;
    if (g_supported && p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, g_dark);
    /* A group box has no dark theme at all: the frame and the caption stay
       dark grey on dark grey, so group_proc draws both. */
    SetWindowTheme(hwnd, NULL, NULL);
    SetWindowSubclass(hwnd, group_proc, SC_GROUP, 0);
    InvalidateRect(hwnd, NULL, TRUE);
}

void Theme_ApplyToEdit(HWND hwnd)
{
    LONG_PTR ex;
    if (!hwnd) return;
    if (g_supported && p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, g_dark);
    /* DarkMode_Explorer is what turns the scroll bars dark. */
    SetWindowTheme(hwnd, g_dark ? L"DarkMode_Explorer" : NULL, NULL);

    /* WS_EX_CLIENTEDGE is a light 3D frame no theme touches. */
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (g_dark) ex &= ~(LONG_PTR)WS_EX_CLIENTEDGE;
    else        ex |=  (LONG_PTR)WS_EX_CLIENTEDGE;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hwnd, NULL, TRUE);
}

/* ============================================================
 *  Painting helpers
 * ============================================================ */

static void fill(HDC dc, const RECT *rc, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, rc, b);
    DeleteObject(b);
}

static void frame(HDC dc, const RECT *rc, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, rc, b);
    DeleteObject(b);
}

/* ============================================================
 *  List view  (only the header needs hand painting)
 * ============================================================ */

static LRESULT list_header_customdraw(HWND list, NMCUSTOMDRAW *cd)
{
    HWND hdr = ListView_GetHeader(list);

    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT: {
        HDITEMW hi;
        wchar_t txt[128];
        RECT rc = cd->rc, tr, s;
        HFONT f = (HFONT)SendMessageW(hdr, WM_GETFONT, 0, 0);
        HFONT old = f ? (HFONT)SelectObject(cd->hdc, f) : NULL;
        BOOL pressed = (cd->uItemState & CDIS_SELECTED) != 0;

        fill(cd->hdc, &rc, pressed ? D_FACE : D_SURFACE);

        s = rc; s.left = s.right - 1;   fill(cd->hdc, &s, D_LINE);
        s = rc; s.top  = s.bottom - 1;  fill(cd->hdc, &s, D_LINE);

        txt[0] = 0;
        ZeroMemory(&hi, sizeof(hi));
        hi.mask = HDI_TEXT;
        hi.pszText = txt;
        hi.cchTextMax = ARRAYSIZE(txt);
        SendMessageW(hdr, HDM_GETITEMW, (WPARAM)cd->dwItemSpec, (LPARAM)&hi);

        tr = rc;
        tr.left += 8;
        tr.right -= 8;
        SetBkMode(cd->hdc, TRANSPARENT);
        SetTextColor(cd->hdc, D_TEXT);
        DrawTextW(cd->hdc, txt, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (old) SelectObject(cd->hdc, old);
        return CDRF_SKIPDEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static LRESULT CALLBACK list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, list_proc, id);
        break;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        /* The header is a child of the list view, so its notifications stop
           here instead of reaching the main window. */
        if (g_dark && nm && nm->code == NM_CUSTOMDRAW &&
            nm->hwndFrom == ListView_GetHeader(hwnd))
            return list_header_customdraw(hwnd, (NMCUSTOMDRAW *)lp);
        break;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ============================================================
 *  Push buttons
 * ============================================================ */

static LRESULT CALLBACK button_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref)
{
    BOOL hot = (BOOL)ref;

    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, button_proc, id);
        break;

    case WM_ERASEBKGND:
        if (g_dark) return 1;
        break;

    /* comctl32 renders these state changes straight into its own DC rather
       than invalidating, so WM_PAINT below never runs and the light themed
       button is left on screen. Force a real repaint afterwards. */
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
    case BM_SETSTATE:
    case BM_SETCHECK:
        if (g_dark) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, NULL, FALSE);
            return r;
        }
        break;

    case WM_MOUSEMOVE:
        if (g_dark && !hot) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            SetWindowSubclass(hwnd, button_proc, id, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_MOUSELEAVE:
        if (hot) {
            SetWindowSubclass(hwnd, button_proc, id, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_PAINT:
        if (g_dark) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc, tr;
            wchar_t txt[128];
            HFONT f = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            HFONT old = f ? (HFONT)SelectObject(dc, f) : NULL;
            BOOL enabled = IsWindowEnabled(hwnd);
            LRESULT st = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
            BOOL pressed = (st & BST_PUSHED) != 0;
            COLORREF face;

            GetClientRect(hwnd, &rc);
            if (!enabled)     face = D_BG;
            else if (pressed) face = D_PRESSED;
            else if (hot)     face = D_HOT;
            else              face = D_FACE;

            fill(dc, &rc, face);
            frame(dc, &rc, enabled ? (hot ? D_ACCENT : D_LINE) : D_LINE);

            txt[0] = 0;
            GetWindowTextW(hwnd, txt, ARRAYSIZE(txt));
            tr = rc;
            tr.left += 4;
            tr.right -= 4;
            if (pressed) OffsetRect(&tr, 1, 1);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? D_TEXT : D_DISABLED);
            DrawTextW(dc, txt, -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_HIDEPREFIX);

            if (enabled && GetFocus() == hwnd) {
                RECT fr = rc;
                InflateRect(&fr, -3, -3);
                SetTextColor(dc, D_TEXT);
                SetBkColor(dc, face);
                DrawFocusRect(dc, &fr);
            }

            if (old) SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ============================================================
 *  Check boxes  (box glyph and label both drawn by hand)
 * ============================================================ */

static void draw_check_glyph(HDC dc, const RECT *box, COLORREF c)
{
    int w = box->right - box->left;
    int thick = w / 7;
    POINT pt[3];
    HPEN pen, old;

    if (thick < 1) thick = 1;
    pt[0].x = box->left + w * 22 / 100;  pt[0].y = box->top + w * 52 / 100;
    pt[1].x = box->left + w * 42 / 100;  pt[1].y = box->top + w * 73 / 100;
    pt[2].x = box->left + w * 79 / 100;  pt[2].y = box->top + w * 28 / 100;

    pen = CreatePen(PS_SOLID, thick, c);
    old = (HPEN)SelectObject(dc, pen);
    Polyline(dc, pt, 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static LRESULT CALLBACK check_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR ref)
{
    BOOL hot = (BOOL)ref;

    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, check_proc, id);
        break;

    case WM_ERASEBKGND:
        if (g_dark) return 1;
        break;

    /* Same as the push buttons: these states are rendered straight into the
       control's own DC, so force a real repaint afterwards. */
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
    case BM_SETSTATE:
    case BM_SETCHECK:
        if (g_dark) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, NULL, FALSE);
            return r;
        }
        break;

    case WM_MOUSEMOVE:
        if (g_dark && !hot) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            SetWindowSubclass(hwnd, check_proc, id, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_MOUSELEAVE:
        if (hot) {
            SetWindowSubclass(hwnd, check_proc, id, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_PAINT:
        if (g_dark) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc, box, tr;
            wchar_t txt[160];
            HFONT f = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            HFONT old = f ? (HFONT)SelectObject(dc, f) : NULL;
            BOOL enabled = IsWindowEnabled(hwnd);
            LRESULT st = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
            BOOL checked = (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
            BOOL pressed = (st & BST_PUSHED) != 0;
            int dpi = GetDeviceCaps(dc, LOGPIXELSX);
            int side = MulDiv(13, dpi, 96);
            int gap  = MulDiv(6, dpi, 96);
            COLORREF textCol = enabled ? D_TEXT : D_DISABLED;

            GetClientRect(hwnd, &rc);
            fill(dc, &rc, D_BG);

            box.left   = rc.left;
            box.top    = rc.top + ((rc.bottom - rc.top) - side) / 2;
            box.right  = box.left + side;
            box.bottom = box.top + side;
            if (box.top < rc.top) box.top = rc.top;

            fill(dc, &box, enabled ? (pressed ? D_PRESSED : D_SURFACE) : D_BG);
            frame(dc, &box, !enabled ? D_LINE : (hot ? D_ACCENT : D_LINE));
            if (checked)
                draw_check_glyph(dc, &box, enabled ? D_ACCENT : D_DISABLED);

            txt[0] = 0;
            GetWindowTextW(hwnd, txt, ARRAYSIZE(txt));
            tr = rc;
            tr.left = box.right + gap;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, textCol);
            DrawTextW(dc, txt, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_HIDEPREFIX);

            if (enabled && GetFocus() == hwnd) {
                RECT fr = tr;
                SIZE ts;
                GetTextExtentPoint32W(dc, txt, (int)wcslen(txt), &ts);
                fr.right = fr.left + ts.cx + 2;
                fr.top    = rc.top + ((rc.bottom - rc.top) - ts.cy) / 2 - 1;
                fr.bottom = fr.top + ts.cy + 2;
                fr.left -= 1;
                SetTextColor(dc, D_TEXT);
                SetBkColor(dc, D_BG);
                DrawFocusRect(dc, &fr);
            }

            if (old) SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ============================================================
 *  Group box  (no dark theme exists, so frame and caption are drawn)
 * ============================================================ */

static LRESULT CALLBACK group_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, group_proc, id);
        break;

    case WM_ERASEBKGND:
        if (g_dark) return 1;
        break;

    case WM_PAINT:
        if (g_dark) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc, fr, tr;
            wchar_t txt[128];
            SIZE ts;
            HFONT f = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            HFONT old = f ? (HFONT)SelectObject(dc, f) : NULL;
            int capH;

            GetClientRect(hwnd, &rc);
            fill(dc, &rc, D_BG);

            txt[0] = 0;
            GetWindowTextW(hwnd, txt, ARRAYSIZE(txt));
            GetTextExtentPoint32W(dc, txt, (int)wcslen(txt), &ts);
            capH = ts.cy;

            /* The frame starts at the caption's vertical middle, the way the
               themed group box draws it. */
            fr = rc;
            fr.top += capH / 2;
            frame(dc, &fr, D_LINE);

            if (txt[0]) {
                RECT gap;
                SetRect(&gap, rc.left + 8, fr.top - 1, rc.left + 8 + ts.cx + 8, fr.top + 1);
                fill(dc, &gap, D_BG);      /* punch a hole for the caption */
                tr = rc;
                tr.left += 12;
                tr.top   = rc.top;
                tr.bottom = rc.top + capH;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, D_TEXT);
                DrawTextW(dc, txt, -1, &tr,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }

            if (old) SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
