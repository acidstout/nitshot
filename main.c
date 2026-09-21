/*
 * main.c - Nitshot.
 *
 * A tray-resident host: it owns the hotkey hook, the tray icon and its menu,
 * and hands every capture request to the capture/overlay code. It deliberately
 * has no visible window of its own - the only UI in normal use is the overlay
 * that appears when a hotkey fires.
 */

#include "nitshot.h"
#include "resource.h"
#include "settings.h"
#include "hotkey.h"
#include "theme.h"
#include "capture.h"
#include "save.h"
#include "overlay.h"
#include "settings_ui.h"
#include "record.h"
#include "encoder.h"
#include "lang.h"
#include "recordbar.h"
#include "log.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>

#define WM_TRAYICON     (WM_APP + 1)
#define TIMER_HOOKHEAL  1
/*
 * A low-level hook can stop working in two ways, neither of which can be
 * detected: Windows drops one whose callback ever failed to answer within
 * LowLevelHooksTimeout, and anything installing a hook after us is called
 * ahead of us and can swallow the keys first. Reinstalling costs two API calls
 * and bounds how long the hotkeys can be silently dead.
 *
 * This is the safety net, not the fix. The failure actually seen here was the
 * first kind and was self-inflicted: a 4K HDR snip spent seconds in the
 * encoder on this thread, the hook could not be dispatched while that ran, and
 * Win+Shift+S went to the shell - which opened Snip & Sketch, the one thing
 * this program exists to prevent. Saving moved to a worker thread because of
 * it; see Save_SnipAsync.
 */
#define HOOKHEAL_MS     10000

#define IDM_SNIP        0x100
#define IDM_FULLSCREEN  0x101
#define IDM_OPENFOLDER  0x102
#define IDM_AUTOSTART   0x103
#define IDM_SETTINGS    0x104
#define IDM_ABOUT       0x105
#define IDM_EXIT        0x106

static HWND      g_host;
static HICON     g_icon;
static UINT      g_msgTaskbarCreated;
static BOOL      g_trayShown;
static wchar_t   g_lastFile[MAX_PATH];   /* what the last balloon points at */

/* ------------------------------------------------------------------ tray */

static void TrayAdd(void)
{
    NOTIFYICONDATAW nid;

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_host;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = g_icon;
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip), L"%s - %s",
                   APP_TITLE_W, Lang_Str(STR_TRAY_TIP));

    g_trayShown = Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;

    if (!g_trayShown)
        return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_host;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayShown = FALSE;
}

static void TrayBalloon(const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid;

    if (!g_trayShown || !g_cfg.showToast)
        return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = g_host;
    nid.uID         = 1;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    StringCchCopyW(nid.szInfo,      ARRAYSIZE(nid.szInfo),      text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ---------------------------------------------------------- recording */

static void App_StartRecording(const RECT *screenRect)
{
    RecordOptions opt;

    ZeroMemory(&opt, sizeof(opt));
    opt.region = *screenRect;
    opt.audio  = (AudioSource)g_cfg.recordAudio;
    opt.fps    = g_cfg.recordFps;

    Log_Printf(L"record: starting %ldx%ld at %d fps, audio=%d",
               screenRect->right - screenRect->left,
               screenRect->bottom - screenRect->top, opt.fps, (int)opt.audio);

    /*
     * The still-capture machinery keeps a duplication open on every output for
     * the whole run, and DXGI will not hand a second one for the same output to
     * the same process - it fails the recorder's DuplicateOutput with
     * E_INVALIDARG. Letting go of it for the duration is the price of
     * recording; the next snip re-creates it lazily.
     */
    Capture_Shutdown();

    if (!Record_Start(g_host, WM_BSNIP_RECORDED, &opt)) {
        Capture_Init();
        TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_RECORD_START));
        return;
    }
    /* The bar is created here rather than by the worker: it is a window, and
       windows belong to the thread that pumps messages for them. */
    RecordBar_Show(g_host, screenRect, WM_BSNIP_STOPREC);
}

static void App_Recorded(RecordResult *res)
{
    wchar_t text[512];

    RecordBar_Hide();
    /* Warm the still-capture path again now the recorder has let the output go. */
    if (!g_cfg.forceGdi)
        Capture_Init();
    if (!res)
        return;

    Log_Printf(L"record: finished ok=%d %lus %s %s", (int)res->ok,
               (unsigned long)res->seconds, res->path, res->error);

    if (res->ok) {
        StringCchCopyW(g_lastFile, ARRAYSIZE(g_lastFile), res->path);
        wchar_t len[16], head[400];
        StringCchPrintfW(len, ARRAYSIZE(len), L"%lu:%02lu",
                         (unsigned long)(res->seconds / 60),
                         (unsigned long)(res->seconds % 60));
        Lang_Format(head, ARRAYSIZE(head), STR_TOAST_RECORDED, len,
                    PathFindFileNameW(res->path));
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s%s", head,
                         res->wavPath[0] ? Lang_Str(STR_TOAST_RECORD_WAV) : L"");
    } else {
        Lang_Format(text, ARRAYSIZE(text), STR_TOAST_RECORD_FAIL, res->error);
    }
    TrayBalloon(APP_TITLE_W, text);
    Record_FreeResult(res);
}

/* ------------------------------------------------------------- the action */

/*
 * Freeze the desktop, let the user pick a region, then publish the result.
 * PrtScn can be configured to skip the overlay entirely and take everything.
 */
static void App_Snip(TriggerSource src)
{
    static BOOL   busy;              /* a second hotkey while the overlay is up */
    SnipImage     frozen, shot;
    SnipSelection sel;
    RECT          bounds;
    LARGE_INTEGER t0, t1, freq;
    double        grabMs;
    BOOL          wholeScreen;
    UINT32        w, h;

    if (busy)
        return;

    /* While a recording runs the hotkey is the way to end it - starting a
       second capture on top would be the wrong thing to want. */
    if (Record_IsActive()) {
        Log_Printf(L"snip: trigger=%d while recording -> stop", (int)src);
        Record_Stop();
        return;
    }

    busy = TRUE;
    Log_Printf(L"snip: trigger=%d", (int)src);

    /* GetTickCount's 15 ms granularity cannot see this at all - the warm
       duplication path is well under one frame. */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    if (!Capture_VirtualDesktop(&frozen, &bounds)) {
        Log_Printf(L"snip: capture FAILED");
        TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_CAPTURE_FAILED));
        busy = FALSE;
        return;
    }
    QueryPerformanceCounter(&t1);
    grabMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    wholeScreen = (src == TRIGGER_FULLSCREEN) ||
                  (src == TRIGGER_PRINTSCREEN && g_cfg.prtScnFullscreen);

    ZeroMemory(&sel, sizeof(sel));
    if (wholeScreen) {
        shot = frozen;                       /* borrowed, not owned */
    } else {
        /* Keep eating the hotkeys while the overlay is up, but act on none of
           them: letting them through would hand Win+Shift+S to the shell. */
        Hotkey_SetSuppressed(TRUE);
        Overlay_Run(&frozen, &bounds, g_cfg.defaultMode, &sel);
        Hotkey_SetSuppressed(FALSE);

        if (!sel.taken) {
            Log_Printf(L"snip: cancelled");
            Overlay_FreeSelection(&sel);
            Snip_FreeImage(&frozen);
            busy = FALSE;
            return;
        }
        if (sel.record) {
            RECT screen = sel.rect;
            OffsetRect(&screen, bounds.left, bounds.top);
            Overlay_FreeSelection(&sel);
            Snip_FreeImage(&frozen);
            App_StartRecording(&screen);
            busy = FALSE;
            return;
        }

        /* Remember the mode for next time, the way the Snipping Tool does.
           Fullscreen is a one-off action, not a mode worth returning to. */
        if (sel.mode != SNIP_FULLSCREEN)
            g_cfg.defaultMode = sel.mode;

        if (!Overlay_ExtractSelection(&frozen, &sel, &shot)) {
            Overlay_FreeSelection(&sel);
            Snip_FreeImage(&frozen);
            busy = FALSE;
            return;
        }
    }

    /* In the whole-screen case 'shot' is the frozen image itself; hand that
       over rather than copying eight megapixels to say the same thing. */
    if (wholeScreen)
        ZeroMemory(&frozen, sizeof(frozen));

    w = shot.width;
    h = shot.height;
    Overlay_FreeSelection(&sel);
    Snip_FreeImage(&frozen);

    Log_Printf(L"snip: %ux%u via %s, grab %.1f ms - encoding in the background",
               w, h, Capture_LastMethod(), grabMs);

    /*
     * Everything expensive happens on a worker from here. The message loop has
     * to stay free: the keyboard hook is dispatched on this thread, and a UI
     * thread busy in the encoder means Windows times the hook out and hands
     * Win+Shift+S to the shell instead.
     */
    if (!Save_SnipAsync(g_host, WM_BSNIP_SAVED, &shot))
        TrayBalloon(APP_TITLE_W, Lang_Str(STR_MSG_SAVE_FAILED));

    busy = FALSE;
}

/* The worker is finished; 'res' is ours to report and free. */
static void App_SnipSaved(SaveResult *res)
{
    wchar_t text[512];
    wchar_t tail[192];

    if (!res)
        return;

    Log_Printf(L"snip: saved clip=%d disk=%d sidecar=%d hdr=%d encode %ld ms %s",
               (int)res->clipboardOk, (int)res->diskOk, (int)res->sidecarOk,
               (int)res->hdr, res->encodeMs, res->diskOk ? res->path : L"");

    StringCchCopyW(g_lastFile, ARRAYSIZE(g_lastFile), res->diskOk ? res->path : L"");

    /* Both destinations are optional, so the balloon has to say which of them
       actually happened rather than claiming a saved file that is not there. */
    if (res->diskOk && res->clipboardOk)
        Lang_Format(tail, ARRAYSIZE(tail), STR_TOAST_COPIED_SAVED,
                    PathFindFileNameW(res->path));
    else if (res->diskOk)
        Lang_Format(tail, ARRAYSIZE(tail), STR_TOAST_SAVED,
                    PathFindFileNameW(res->path));
    else if (res->clipboardOk)
        StringCchCopyW(tail, ARRAYSIZE(tail), Lang_Str(STR_TOAST_COPIED));
    else
        StringCchCopyW(tail, ARRAYSIZE(tail), Lang_Str(STR_TOAST_NOTHING));

    {
        wchar_t ms[24], encoded[64];
        StringCchPrintfW(ms, ARRAYSIZE(ms), L"%ld", res->encodeMs);
        Lang_Format(encoded, ARRAYSIZE(encoded), STR_TOAST_ENCODE, ms);
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s%s%s", tail,
                         res->hdr ? Lang_Str(res->sidecarOk ? STR_TOAST_HDR_SDR
                                                            : STR_TOAST_HDR)
                                  : L"",
                         encoded);
    }
    TrayBalloon(APP_TITLE_W, text);
    Save_FreeResult(res);
}

static void App_OpenFolder(void)
{
    wchar_t dir[MAX_PATH];

    if (Settings_ResolveSaveFolder(dir, ARRAYSIZE(dir)))
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    else
        MessageBoxW(NULL, Lang_Str(STR_MSG_FOLDER_FAILED),
                    APP_TITLE_W, MB_OK | MB_ICONWARNING);
}

/* Clicking the balloon shows the file it just named. */
static void App_RevealLastFile(void)
{
    wchar_t args[MAX_PATH + 16];

    if (!g_lastFile[0]) {
        App_OpenFolder();
        return;
    }
    StringCchPrintfW(args, ARRAYSIZE(args), L"/select,\"%s\"", g_lastFile);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static void App_About(void)
{
    wchar_t dir[MAX_PATH] = L"(unavailable)";
    wchar_t text[768];

    wchar_t body[640];

    Settings_ResolveSaveFolder(dir, ARRAYSIZE(dir));
    Lang_Format(body, ARRAYSIZE(body), STR_ABOUT_BODY, dir);
    /* The name and version line is product identity, not prose, and stays put
       in every language. */
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s  %s\n%s",
                     APP_TITLE_W, APP_VER_WSTRING, body);
    MessageBoxW(NULL, text, APP_TITLE_W, MB_OK | MB_ICONINFORMATION);
}

static void ShowTrayMenu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, IDM_SNIP,       Lang_Str(STR_MENU_SNIP));
    AppendMenuW(menu, MF_STRING, IDM_FULLSCREEN, Lang_Str(STR_MENU_FULLSCREEN));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_OPENFOLDER, Lang_Str(STR_MENU_OPENFOLDER));
    AppendMenuW(menu, MF_STRING | (Settings_GetAutostart() ? MF_CHECKED : 0u),
                IDM_AUTOSTART, Lang_Str(STR_MENU_AUTOSTART));
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, Lang_Str(STR_MENU_SETTINGS));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, Lang_Str(STR_MENU_ABOUT));
    AppendMenuW(menu, MF_STRING, IDM_EXIT,  Lang_Str(STR_MENU_EXIT));
    SetMenuDefaultItem(menu, IDM_SNIP, FALSE);

    /* The menu only dismisses on an outside click while its owner is the
       foreground window, and a tray owner never is by itself. */
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_host, NULL);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ---------------------------------------------------------- window proc */

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        g_trayShown = FALSE;      /* Explorer restarted; our icon went with it */
        TrayAdd();
        return 0;
    }

    switch (msg) {
    case WM_BSNIP_TRIGGER:
        App_Snip((TriggerSource)wp);
        return 0;

    case WM_BSNIP_SAVED:
        App_SnipSaved((SaveResult *)lp);
        return 0;

    case WM_BSNIP_RECORDED:
        App_Recorded((RecordResult *)lp);
        return 0;

    case WM_BSNIP_STOPREC:
        Record_Stop();
        return 0;

    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK:      App_Snip(TRIGGER_TRAY);  return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:        ShowTrayMenu();          return 0;
        case NIN_BALLOONUSERCLICK:  App_RevealLastFile();    return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SNIP:       App_Snip(TRIGGER_TRAY);       break;
        case IDM_FULLSCREEN: App_Snip(TRIGGER_FULLSCREEN); break;
        case IDM_OPENFOLDER: App_OpenFolder();       break;
        case IDM_AUTOSTART:  Settings_SetAutostart(!Settings_GetAutostart()); break;
        case IDM_SETTINGS:   SettingsUI_Show(NULL);  break;
        case IDM_ABOUT:      App_About();            break;
        case IDM_EXIT:       DestroyWindow(hwnd);    break;
        default:                                     break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_HOOKHEAL)
            Hotkey_Refresh();
        return 0;

    case WM_SETTINGCHANGE:
        if (Theme_IsColorSchemeChange(msg, lp))
            Theme_SetDark(Theme_SystemPrefersDark());
        return 0;

    case WM_DESTROY:
        Record_Stop();
        RecordBar_Hide();
        KillTimer(hwnd, TIMER_HOOKHEAL);
        Hotkey_Uninstall();
        TrayRemove();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------- startup */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    HANDLE      mutex;
    MSG         msg;

    UNREFERENCED_PARAMETER(prev);
    UNREFERENCED_PARAMETER(cmdline);
    UNREFERENCED_PARAMETER(show);

    /* A second copy is a request to capture, not a reason to run twice. */
    mutex = CreateMutexW(NULL, TRUE, APP_MUTEX_W);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS_W, NULL);
        if (other)
            PostMessageW(other, WM_BSNIP_TRIGGER, TRIGGER_SECOND_INSTANCE, 0);
        CloseHandle(mutex);
        return 0;
    }

    /* WIC and the shell verbs both need an STA. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Settings_Load();
    Settings_Save();   /* write the defaults back, so the INI is there to edit */
    Log_Enable(g_cfg.debugLog);
    Log_Printf(L"---- start ----");
    /* Before any window exists, so nothing is ever built in the wrong
       language and then corrected. */
    Lang_Init(g_cfg.language);
    Theme_Init();
    Theme_SetDark(Theme_SystemPrefersDark());

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                               0, 0, LR_DEFAULTSIZE | LR_SHARED);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = inst;
    wc.hIcon         = g_icon;
    wc.lpszClassName = APP_CLASS_W;
    if (!RegisterClassExW(&wc))
        return 1;

    /* Never shown: it exists to receive the tray callback and the hotkey post.
       HWND_MESSAGE would be tidier but cannot receive the TaskbarCreated
       broadcast, which is how the icon comes back when Explorer restarts. */
    g_host = CreateWindowExW(0, APP_CLASS_W, APP_TITLE_W, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_host)
        return 1;

    TrayAdd();
    Save_Init();
    /* Built up front so the first hotkey press does not pay for device
       creation; a failure here is not fatal, capture falls back to GDI. */
    if (!g_cfg.forceGdi)
        Capture_Init();

    /* Finding out which video encoders work means writing and finalising a
       file per candidate - seconds, not milliseconds - so it runs on a worker
       and the answer is cached. Doing it now means the first recording does
       not stall, and doing it off the message loop means startup does not. */
    Encoder_ProbeAsync();

    Log_Printf(L"capture outputs ready, hook installing");
    if (!Hotkey_Install(g_host)) {
        Log_Printf(L"hook install FAILED (%lu)", GetLastError());
        wchar_t warn[512];
        Lang_Format(warn, ARRAYSIZE(warn), STR_MSG_HOOK_FAILED, APP_TITLE_W);
        MessageBoxW(NULL, warn, APP_TITLE_W, MB_OK | MB_ICONWARNING);
    }
    SetTimer(g_host, TIMER_HOOKHEAL, HOOKHEAL_MS, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Settings_Save();     /* the mode the user last picked */
    Capture_Shutdown();
    Save_Shutdown();
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return (int)msg.wParam;
}
