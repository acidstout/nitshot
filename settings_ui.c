/*
 * settings_ui.c - the settings dialog.
 *
 * Everything here is also reachable by editing settings.ini by hand; the dialog
 * exists so that the options are discoverable at all. It follows the Windows
 * light/dark setting through theme.c, because a tray tool that opens a blazing
 * white window on a dark desktop looks broken.
 */

#define COBJMACROS

#include "nitshot.h"
#include "resource.h"
#include "settings.h"
#include "settings_ui.h"
#include "capture.h"
#include "hdr.h"
#include "encoder.h"
#include "lang.h"
#include "log.h"
#include "tabs.h"
#include "theme.h"

#include <shlobj.h>
#include <strsafe.h>

/* Where the dialog was put, so a later move can be told from no move at all. */
static POINT g_placedAt;

/*
 * Centre on the monitor the pointer is on, or restore where the user last
 * dragged it. A saved position is only honoured while it still lands on a
 * monitor - unplugging a display would otherwise open the dialog off-screen,
 * with no way back to it.
 */
static void PlaceDialog(HWND dlg)
{
    RECT    r;
    int     w, h, x = 0, y = 0;
    BOOL    restored = FALSE;

    if (!GetWindowRect(dlg, &r))
        return;
    w = r.right - r.left;
    h = r.bottom - r.top;

    if (g_cfg.settingsX != SETTINGS_POS_UNSET &&
        g_cfg.settingsY != SETTINGS_POS_UNSET) {
        RECT want;
        SetRect(&want, g_cfg.settingsX, g_cfg.settingsY,
                g_cfg.settingsX + w, g_cfg.settingsY + h);
        if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) {
            x = g_cfg.settingsX;
            y = g_cfg.settingsY;
            restored = TRUE;
        }
    }

    if (!restored) {
        POINT       pt;
        MONITORINFO mi;
        RECT        work;

        GetCursorPos(&pt);
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi))
            work = mi.rcWork;
        else
            SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                    GetSystemMetrics(SM_CYSCREEN));

        x = work.left + ((work.right - work.left) - w) / 2;
        y = work.top  + ((work.bottom - work.top) - h) / 2;
        if (y < work.top)
            y = work.top;       /* a tall dialog on a short screen */
    }

    SetWindowPos(dlg, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_placedAt.x = x;
    g_placedAt.y = y;
}

/* Only a position the user actually changed is worth remembering; leaving it
   unset keeps the dialog centred as the desktop changes. */
static void SaveDialogPos(HWND dlg)
{
    RECT r;

    if (!GetWindowRect(dlg, &r))
        return;
    if (r.left == g_placedAt.x && r.top == g_placedAt.y)
        return;
    g_cfg.settingsX = r.left;
    g_cfg.settingsY = r.top;
}

/* ------------------------------------------------------------------ pages */

enum { PAGE_GENERAL, PAGE_HDR, PAGE_RECORD, PAGE_HOTKEYS, PAGE_ADVANCED,
       PAGE_COUNT };

/*
 * Which controls belong to which tab. All of them live in the one dialog and
 * share the area below the strip; switching tabs only shows one set and hides
 * the rest. That keeps every control addressable through the dialog as
 * before, so loading, storing and enabling need no knowledge of tabs at all.
 */
static const int g_pages[PAGE_COUNT][16] = {
    { IDC_COPYCLIP, IDC_SAVEDISK, IDC_LBL_FOLDER, IDC_FOLDER, IDC_BROWSE,
      IDC_LBL_VIDFOLDER, IDC_VIDFOLDER, IDC_VIDBROWSE, IDC_LBL_DIM, IDC_DIM,
      IDC_LBL_PERCENT, IDC_TOAST, IDC_LBL_LANGUAGE, IDC_LANGUAGE,
      IDC_AUTOSTART, 0 },
    { IDC_HDRSTATUS, IDC_JXR, IDC_SIDECAR, IDC_ROLLOFF, IDC_LBL_QUALITY,
      IDC_QUALITY, IDC_RECHDR, IDC_FORCESDR, 0 },
    { IDC_LBL_SOUND, IDC_RECAUDIO, IDC_LBL_FPS, IDC_RECFPS, IDC_LBL_VIDEO,
      IDC_RECENC, IDC_RECWAV, IDC_RECMUX, 0 },
    { IDC_HOOKWSS, IDC_HOOKPRTSC, IDC_PRTSCFULL, 0 },
    { IDC_FORCEGDI, IDC_DEBUGLOG, 0 },
};

/* Reopening the dialog returns to the tab it was closed on. */
static int g_page;

static void ShowPage(HWND dlg, int page)
{
    int p, i;

    if (page < 0 || page >= PAGE_COUNT)
        page = PAGE_GENERAL;
    /* Hide first, then show: showing the new set first would briefly draw
       two pages over each other. */
    for (p = 0; p < PAGE_COUNT; p++)
        if (p != page)
            for (i = 0; g_pages[p][i]; i++)
                ShowWindow(GetDlgItem(dlg, g_pages[p][i]), SW_HIDE);
    for (i = 0; g_pages[page][i]; i++)
        ShowWindow(GetDlgItem(dlg, g_pages[page][i]), SW_SHOW);
    g_page = page;
}

/*
 * Ctrl+Tab and Ctrl+Shift+Tab switch tabs, as in every tabbed dialog. The
 * modal loop's IsDialogMessage would otherwise treat them as plain Tab, so a
 * message filter hook - the documented way into a dialog's own loop - picks
 * them off first. Ctrl+PgDn / Ctrl+PgUp do the same.
 */
static HHOOK g_filter;
static HWND  g_filterDlg;

static LRESULT CALLBACK DialogFilter(int code, WPARAM wp, LPARAM lp)
{
    MSG *m = (MSG *)lp;

    if (code == MSGF_DIALOGBOX && m && m->message == WM_KEYDOWN &&
        (GetKeyState(VK_CONTROL) & 0x8000) && g_filterDlg &&
        (m->hwnd == g_filterDlg || IsChild(g_filterDlg, m->hwnd))) {
        int step = 0;
        if (m->wParam == VK_TAB)
            step = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
        else if (m->wParam == VK_NEXT)
            step = 1;
        else if (m->wParam == VK_PRIOR)
            step = -1;
        if (step) {
            HWND tabs = GetDlgItem(g_filterDlg, IDC_TABS);
            Tabs_SetSel(tabs, (Tabs_GetSel(tabs) + step + PAGE_COUNT) % PAGE_COUNT);
            return 1;
        }
    }
    return CallNextHookEx(g_filter, code, wp, lp);
}

/*
 * Every piece of fixed text in the dialog, paired with the string that
 * replaces it. The .rc still carries the English so the layout can be seen in
 * a resource editor, but what is on screen always comes from here - the same
 * path for English as for anything else, so English cannot silently become the
 * only one that was tested.
 */
static void TranslateDialog(HWND dlg)
{
    static const struct { int id; StringId str; } map[] = {
        { IDC_COPYCLIP,      STR_UI_COPYCLIP  },
        { IDC_SAVEDISK,      STR_UI_SAVEDISK  },
        { IDC_LBL_FOLDER,    STR_UI_FOLDER    },
        { IDC_BROWSE,        STR_UI_BROWSE    },
        { IDC_LBL_VIDFOLDER, STR_UI_VIDFOLDER },
        { IDC_VIDBROWSE,     STR_UI_VIDBROWSE },
        { IDC_LBL_DIM,       STR_UI_DIM       },
        { IDC_LBL_PERCENT,   STR_UI_PERCENT   },
        { IDC_TOAST,         STR_UI_TOAST     },
        { IDC_LBL_LANGUAGE,  STR_UI_LANGUAGE  },
        { IDC_AUTOSTART,     STR_UI_AUTOSTART },
        { IDC_JXR,           STR_UI_JXR       },
        { IDC_SIDECAR,       STR_UI_SIDECAR   },
        { IDC_ROLLOFF,       STR_UI_ROLLOFF   },
        { IDC_LBL_QUALITY,   STR_UI_QUALITY   },
        { IDC_RECHDR,        STR_UI_RECHDR    },
        { IDC_FORCESDR,      STR_UI_FORCESDR  },
        { IDC_LBL_SOUND,     STR_UI_RECAUDIO  },
        { IDC_LBL_FPS,       STR_UI_RECFPS    },
        { IDC_LBL_VIDEO,     STR_UI_RECENC    },
        { IDC_RECWAV,        STR_UI_RECWAV    },
        { IDC_RECMUX,        STR_UI_RECMUX    },
        { IDC_HOOKWSS,       STR_UI_HOOKWSS   },
        { IDC_HOOKPRTSC,     STR_UI_HOOKPRTSC },
        { IDC_PRTSCFULL,     STR_UI_PRTSCFULL },
        { IDC_FORCEGDI,      STR_UI_FORCEGDI  },
        { IDC_DEBUGLOG,      STR_UI_DEBUGLOG  },
        { IDOK,              STR_UI_OK        },
        { IDCANCEL,          STR_UI_CANCEL    },
        { IDC_APPLY,         STR_UI_APPLY     },
    };
    const wchar_t *tabs[PAGE_COUNT];
    int i;

    SetWindowTextW(dlg, Lang_Str(STR_DLG_CAPTION));
    for (i = 0; i < (int)ARRAYSIZE(map); i++)
        SetDlgItemTextW(dlg, map[i].id, Lang_Str(map[i].str));

    tabs[PAGE_GENERAL]  = Lang_Str(STR_TAB_GENERAL);
    tabs[PAGE_HDR]      = Lang_Str(STR_TAB_HDR);
    tabs[PAGE_RECORD]   = Lang_Str(STR_TAB_RECORD);
    tabs[PAGE_HOTKEYS]  = Lang_Str(STR_TAB_HOTKEYS);
    tabs[PAGE_ADVANCED] = Lang_Str(STR_TAB_ADVANCED);
    Tabs_SetLabels(GetDlgItem(dlg, IDC_TABS), tabs, PAGE_COUNT);
}

static void SetCheck(HWND dlg, int id, BOOL on)
{
    CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED);
}

static BOOL GetCheck(HWND dlg, int id)
{
    return IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

/* Grey out the options that only mean something when their parent is on. */
static void UpdateEnabling(HWND dlg)
{
    BOOL disk = GetCheck(dlg, IDC_SAVEDISK);
    BOOL jxr  = GetCheck(dlg, IDC_JXR);

    /* Only the Pictures row: recordings are always written to disk, so the
       Videos folder matters whatever "Save files" says. */
    EnableWindow(GetDlgItem(dlg, IDC_LBL_FOLDER), disk);
    EnableWindow(GetDlgItem(dlg, IDC_FOLDER),  disk);
    EnableWindow(GetDlgItem(dlg, IDC_BROWSE),  disk);
    EnableWindow(GetDlgItem(dlg, IDC_JXR),     disk);
    EnableWindow(GetDlgItem(dlg, IDC_SIDECAR), disk && jxr);
    EnableWindow(GetDlgItem(dlg, IDC_QUALITY), disk && jxr);
    EnableWindow(GetDlgItem(dlg, IDC_HOOKPRTSC), TRUE);
    EnableWindow(GetDlgItem(dlg, IDC_PRTSCFULL),
                 GetCheck(dlg, IDC_HOOKPRTSC));

    /* Building the MP4's sound from the sidecar needs a sidecar, and there is
       no sound at all to do it with when the source is None. */
    {
        BOOL haveSound = SendDlgItemMessageW(dlg, IDC_RECAUDIO, CB_GETCURSEL,
                                             0, 0) != 0;
        EnableWindow(GetDlgItem(dlg, IDC_RECWAV), haveSound);
        EnableWindow(GetDlgItem(dlg, IDC_RECMUX),
                     haveSound && GetCheck(dlg, IDC_RECWAV));
    }
}

static void ShowHdrStatus(HWND dlg)
{
    wchar_t text[160];
    HdrInfo info;

    ZeroMemory(&info, sizeof(info));
    if (Hdr_AnyDisplayActive()) {
        /* The white level belongs to a monitor, so report the one the mouse is
           on - with one display that is simply the display. */
        POINT       pt;
        MONITORINFOEXW mi;
        GetCursorPos(&pt);
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY),
                            (MONITORINFO *)&mi))
            Hdr_QueryMonitor(mi.szDevice, &info);

        wchar_t nits[16];
        StringCchPrintfW(nits, ARRAYSIZE(nits), L"%.0f", info.sdrWhiteNits);
        Lang_Format(text, ARRAYSIZE(text), STR_HDR_STATUS_ON, nits);
    } else {
        StringCchCopyW(text, ARRAYSIZE(text), Lang_Str(STR_HDR_STATUS_OFF));
    }
    SetDlgItemTextW(dlg, IDC_HDRSTATUS, text);
}

static void FillAudioBox(HWND dlg)
{
    HWND box = GetDlgItem(dlg, IDC_RECAUDIO);
    int  i;

    if (!box)
        return;
    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    /* The order matches AudioSource, so the index is the setting. */
    SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)Lang_Str(STR_AUDIO_NONE));
    SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)Lang_Str(STR_AUDIO_SYSTEM));
    SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)Lang_Str(STR_AUDIO_MIC));
    i = g_cfg.recordAudio;
    if (i < 0 || i > 2)
        i = 1;
    SendMessageW(box, CB_SETCURSEL, (WPARAM)i, 0);
}

/*
 * Only encoders that actually probed clean are offered. The probe is what
 * separates "the driver advertises it" from "a file comes out": the AV1
 * encoder on this machine passes the first test and fails the second.
 *
 * Probing costs a couple of seconds, so it runs here, where opening a settings
 * dialog already implies a wait, rather than at startup.
 */
static void FillEncoderBox(HWND dlg)
{
    HWND               box = GetDlgItem(dlg, IDC_RECENC);
    const EncoderCaps *caps;
    EncoderId          chosen = Encoder_FromToken(g_cfg.recordEncoder);
    EncoderId          id;
    int                sel = 0;

    if (!box)
        return;

    Encoder_Probe(FALSE);
    caps = Encoder_Caps();

    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)Lang_Str(STR_ENC_AUTO));
    SendMessageW(box, CB_SETITEMDATA, 0, (LPARAM)ENC_AUTO);

    for (id = ENC_H264; id < ENC_COUNT; id++) {
        static const StringId names[ENC_COUNT] = {
            STR_ENC_AUTO, STR_ENC_H264, STR_ENC_HEVC8, STR_ENC_HEVC10
        };
        wchar_t label[160];
        int     at;

        if (!caps->usable[id])
            continue;
        /* Naming the MFT is the difference between "HEVC" and knowing the
           card is doing it rather than the CPU. The MFT's own name is a
           Windows string and is left exactly as the driver reports it. */
        StringCchPrintfW(label, ARRAYSIZE(label), L"%s  -  %s%s",
                         Lang_Str(names[id]),
                         caps->mft[id][0] ? caps->mft[id]
                                          : Lang_Str(STR_ENC_SOFTWARE),
                         caps->hardware[id] ? Lang_Str(STR_ENC_HARDWARE) : L"");
        at = (int)SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)label);
        if (at < 0)
            continue;
        SendMessageW(box, CB_SETITEMDATA, (WPARAM)at, (LPARAM)id);
        if (id == chosen)
            sel = at;
    }
    SendMessageW(box, CB_SETCURSEL, (WPARAM)sel, 0);
}

/*
 * Whatever is in langs\, plus "follow Windows". A file dropped in that folder
 * shows up here without a rebuild, which is the whole point of shipping the
 * translations as files rather than as resources.
 */
static wchar_t g_langCodes[32][8];   /* row 1..n; row 0 is "Automatic" */
static int     g_langCount;
/* The language the dialog opened with, so Cancel can put it back - the switch
   happens live, and Cancel has to undo it like any other edit. */
static wchar_t g_langOnOpen[16];

static void FillLanguageBox(HWND dlg)
{
    HWND    box = GetDlgItem(dlg, IDC_LANGUAGE);
    wchar_t codes[32][8];
    int     n, i, sel = 0;

    if (!box)
        return;

    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)Lang_Str(STR_LANG_AUTO));
    SendMessageW(box, CB_SETITEMDATA, 0, 0);

    n = Lang_List(codes, (int)ARRAYSIZE(codes));
    g_langCount = 0;
    for (i = 0; i < n; i++) {
        wchar_t label[64], native[64];
        int     at;

        /* Windows knows what each language calls itself, which beats a table
           we would have to maintain: "Deutsch", not "German". */
        if (GetLocaleInfoEx(codes[i], LOCALE_SNATIVELANGUAGENAME, native,
                            ARRAYSIZE(native)) > 0)
            StringCchPrintfW(label, ARRAYSIZE(label), L"%s  (%s)", native, codes[i]);
        else
            StringCchCopyW(label, ARRAYSIZE(label), codes[i]);

        at = (int)SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)label);
        if (at < 0)
            continue;
        /* The row carries an index into g_langCodes, so the tag survives the
           list being sorted or a language disappearing between openings. */
        StringCchCopyW(g_langCodes[g_langCount], 8, codes[i]);
        g_langCount++;
        SendMessageW(box, CB_SETITEMDATA, (WPARAM)at, (LPARAM)g_langCount);
        if (_wcsicmp(g_cfg.language, codes[i]) == 0)
            sel = at;
    }
    SendMessageW(box, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void LoadIntoDialog(HWND dlg)
{
    wchar_t folder[MAX_PATH];

    SetCheck(dlg, IDC_COPYCLIP,  g_cfg.copyToClipboard);
    SetCheck(dlg, IDC_SAVEDISK,  g_cfg.saveToDisk);

    /* An empty SaveFolder means the default; show where that actually is
       rather than an empty box the user cannot interpret. */
    if (g_cfg.saveFolder[0])
        StringCchCopyW(folder, ARRAYSIZE(folder), g_cfg.saveFolder);
    else if (!Settings_ResolveSaveFolder(folder, ARRAYSIZE(folder)))
        folder[0] = 0;
    SetDlgItemTextW(dlg, IDC_FOLDER, folder);

    if (g_cfg.videoFolder[0])
        StringCchCopyW(folder, ARRAYSIZE(folder), g_cfg.videoFolder);
    else if (!Settings_ResolveVideoFolder(folder, ARRAYSIZE(folder)))
        folder[0] = 0;
    SetDlgItemTextW(dlg, IDC_VIDFOLDER, folder);

    SetCheck(dlg, IDC_HOOKWSS,   g_cfg.hookWinShiftS);
    SetCheck(dlg, IDC_HOOKPRTSC, g_cfg.hookPrintScreen);
    SetCheck(dlg, IDC_PRTSCFULL, g_cfg.prtScnFullscreen);

    SetDlgItemInt(dlg, IDC_DIM, (UINT)g_cfg.dimPercent, FALSE);
    SetCheck(dlg, IDC_TOAST, g_cfg.showToast);

    SetCheck(dlg, IDC_JXR,     g_cfg.hdrSaveJxr);
    SetCheck(dlg, IDC_SIDECAR, g_cfg.hdrSdrSidecar);
    SetCheck(dlg, IDC_ROLLOFF, g_cfg.hdrSdrRollOff);
    SetDlgItemInt(dlg, IDC_QUALITY, (UINT)g_cfg.hdrJxrQuality, FALSE);

    FillAudioBox(dlg);
    SetDlgItemInt(dlg, IDC_RECFPS, (UINT)g_cfg.recordFps, FALSE);
    FillEncoderBox(dlg);
    FillLanguageBox(dlg);
    SetCheck(dlg, IDC_RECHDR, g_cfg.recordHdr);
    SetCheck(dlg, IDC_RECWAV, g_cfg.recordWavSidecar);
    SetCheck(dlg, IDC_RECMUX, g_cfg.recordMuxFromWav);

    SetCheck(dlg, IDC_AUTOSTART, Settings_GetAutostart());
    SetCheck(dlg, IDC_DEBUGLOG,  g_cfg.debugLog);
    SetCheck(dlg, IDC_FORCEGDI,  g_cfg.forceGdi);
    SetCheck(dlg, IDC_FORCESDR,  g_cfg.forceSdr);

    ShowHdrStatus(dlg);
    UpdateEnabling(dlg);
}

static BOOL StoreFromDialog(HWND dlg)
{
    wchar_t folder[MAX_PATH], deflt[MAX_PATH];
    BOOL    ok = FALSE;
    UINT    n;

    g_cfg.copyToClipboard = GetCheck(dlg, IDC_COPYCLIP);
    g_cfg.saveToDisk      = GetCheck(dlg, IDC_SAVEDISK);

    /*
     * Store the default as "empty" so it keeps following the known folder if
     * the profile ever moves. The comparison has to be against the real
     * default: comparing against the resolved setting, as this once did,
     * matched any unchanged custom folder too and quietly reset it.
     */
    GetDlgItemTextW(dlg, IDC_FOLDER, folder, ARRAYSIZE(folder));
    if (Settings_DefaultFolder(FALSE, deflt, ARRAYSIZE(deflt)) &&
        _wcsicmp(folder, deflt) == 0)
        folder[0] = 0;
    StringCchCopyW(g_cfg.saveFolder, ARRAYSIZE(g_cfg.saveFolder), folder);

    GetDlgItemTextW(dlg, IDC_VIDFOLDER, folder, ARRAYSIZE(folder));
    if (Settings_DefaultFolder(TRUE, deflt, ARRAYSIZE(deflt)) &&
        _wcsicmp(folder, deflt) == 0)
        folder[0] = 0;
    StringCchCopyW(g_cfg.videoFolder, ARRAYSIZE(g_cfg.videoFolder), folder);

    g_cfg.hookWinShiftS   = GetCheck(dlg, IDC_HOOKWSS);
    g_cfg.hookPrintScreen = GetCheck(dlg, IDC_HOOKPRTSC);
    g_cfg.prtScnFullscreen= GetCheck(dlg, IDC_PRTSCFULL);

    n = GetDlgItemInt(dlg, IDC_DIM, &ok, FALSE);
    if (ok) {
        if (n > 90) n = 90;
        g_cfg.dimPercent = (int)n;
    }
    g_cfg.showToast = GetCheck(dlg, IDC_TOAST);

    g_cfg.hdrSaveJxr    = GetCheck(dlg, IDC_JXR);
    g_cfg.hdrSdrSidecar = GetCheck(dlg, IDC_SIDECAR);
    g_cfg.hdrSdrRollOff = GetCheck(dlg, IDC_ROLLOFF);
    n = GetDlgItemInt(dlg, IDC_QUALITY, &ok, FALSE);
    if (ok) {
        if (n < 1)   n = 1;
        if (n > 100) n = 100;
        g_cfg.hdrJxrQuality = (int)n;
    }

    {
        LRESULT sel = SendDlgItemMessageW(dlg, IDC_RECAUDIO, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR)
            g_cfg.recordAudio = (int)sel;
    }
    n = GetDlgItemInt(dlg, IDC_RECFPS, &ok, FALSE);
    if (ok) {
        if (n < 5)  n = 5;
        if (n > 60) n = 60;
        g_cfg.recordFps = (int)n;
    }

    {
        /* Stored as the item's EncoderId rather than its position, so the
           setting survives a driver change that shortens the list. */
        LRESULT sel = SendDlgItemMessageW(dlg, IDC_RECENC, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            LRESULT id = SendDlgItemMessageW(dlg, IDC_RECENC, CB_GETITEMDATA,
                                             (WPARAM)sel, 0);
            if (id != CB_ERR && id >= ENC_AUTO && id < ENC_COUNT)
                StringCchCopyW(g_cfg.recordEncoder,
                               ARRAYSIZE(g_cfg.recordEncoder),
                               Encoder_Token((EncoderId)id));
        }
    }
    g_cfg.recordHdr = GetCheck(dlg, IDC_RECHDR);
    g_cfg.recordWavSidecar = GetCheck(dlg, IDC_RECWAV);

    {
        LRESULT sel = SendDlgItemMessageW(dlg, IDC_LANGUAGE, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            LRESULT row = SendDlgItemMessageW(dlg, IDC_LANGUAGE, CB_GETITEMDATA,
                                              (WPARAM)sel, 0);
            if (row == 0)
                StringCchCopyW(g_cfg.language, ARRAYSIZE(g_cfg.language), L"auto");
            else if (row > 0 && row <= g_langCount)
                StringCchCopyW(g_cfg.language, ARRAYSIZE(g_cfg.language),
                               g_langCodes[row - 1]);
        }
    }
    g_cfg.recordMuxFromWav = GetCheck(dlg, IDC_RECMUX);

    g_cfg.debugLog = GetCheck(dlg, IDC_DEBUGLOG);
    g_cfg.forceGdi = GetCheck(dlg, IDC_FORCEGDI);
    g_cfg.forceSdr = GetCheck(dlg, IDC_FORCESDR);

    Settings_SetAutostart(GetCheck(dlg, IDC_AUTOSTART));
    Settings_Save();

    /* Everything else is read where it is used, so it is live the moment it
       is stored. The log is the exception - it was only switched on at
       startup, so ticking it did nothing until the next launch. */
    Log_Enable(g_cfg.debugLog);
    return TRUE;
}

/* The modern folder picker; the old SHBrowseForFolder tree looks its age. */
static void BrowseForFolder(HWND dlg, int editId, StringId title)
{
    IFileDialog *fd = NULL;
    IShellItem  *item = NULL, *start = NULL;
    PWSTR        path = NULL;
    DWORD        opts = 0;
    wchar_t      current[MAX_PATH];

    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileDialog, (void **)&fd)))
        return;

    if (SUCCEEDED(IFileDialog_GetOptions(fd, &opts)))
        IFileDialog_SetOptions(fd, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileDialog_SetTitle(fd, Lang_Str(title));

    /* Open where the setting already points, not wherever the picker was
       last used. */
    GetDlgItemTextW(dlg, editId, current, ARRAYSIZE(current));
    if (current[0] &&
        SUCCEEDED(SHCreateItemFromParsingName(current, NULL, &IID_IShellItem,
                                              (void **)&start))) {
        IFileDialog_SetFolder(fd, start);
        IShellItem_Release(start);
    }

    if (SUCCEEDED(IFileDialog_Show(fd, dlg)) &&
        SUCCEEDED(IFileDialog_GetResult(fd, &item))) {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            SetDlgItemTextW(dlg, editId, path);
            CoTaskMemFree(path);
        }
        IShellItem_Release(item);
    }
    IFileDialog_Release(fd);
}

static void ApplyTheme(HWND dlg)
{
    static const int checks[] = {
        IDC_COPYCLIP, IDC_SAVEDISK, IDC_HOOKWSS, IDC_HOOKPRTSC, IDC_PRTSCFULL,
        IDC_TOAST, IDC_JXR, IDC_SIDECAR, IDC_ROLLOFF,
        IDC_AUTOSTART, IDC_DEBUGLOG, IDC_FORCEGDI, IDC_FORCESDR, IDC_RECHDR,
        IDC_RECWAV, IDC_RECMUX
    };
    static const int edits[]   = { IDC_FOLDER, IDC_VIDFOLDER, IDC_DIM,
                                   IDC_QUALITY, IDC_RECFPS };
    static const int buttons[] = { IDOK, IDCANCEL, IDC_APPLY, IDC_BROWSE,
                                   IDC_VIDBROWSE };
    int i;

    Theme_ApplyToMainWindow(dlg);
    for (i = 0; i < (int)ARRAYSIZE(checks);  i++) Theme_ApplyToCheckBox(GetDlgItem(dlg, checks[i]));
    for (i = 0; i < (int)ARRAYSIZE(edits);   i++) Theme_ApplyToEdit(GetDlgItem(dlg, edits[i]));
    for (i = 0; i < (int)ARRAYSIZE(buttons); i++) Theme_ApplyToButton(GetDlgItem(dlg, buttons[i]));
    /* The strip reads the theme colours when it paints. */
    InvalidateRect(GetDlgItem(dlg, IDC_TABS), NULL, TRUE);
}

/* Set while the dialog fills itself in, so its own changes are not taken
   for the user's. */
static BOOL g_loading;

static BOOL IsPageControl(int id)
{
    int p, i;
    for (p = 0; p < PAGE_COUNT; p++)
        for (i = 0; g_pages[p][i]; i++)
            if (g_pages[p][i] == id)
                return TRUE;
    return FALSE;
}

/* Apply is only offered while there is something to apply. */
static void SetDirty(HWND dlg, BOOL dirty)
{
    HWND apply = GetDlgItem(dlg, IDC_APPLY);

    /* A button must not keep the focus while it is being disabled, or the
       keyboard is left pointing at nothing. */
    if (!dirty && GetFocus() == apply)
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dlg, IDOK), TRUE);
    EnableWindow(apply, dirty);
}

static INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                     (LPARAM)LoadImageW(GetModuleHandleW(NULL),
                                        MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                        0, 0, LR_DEFAULTSIZE | LR_SHARED));
        StringCchCopyW(g_langOnOpen, ARRAYSIZE(g_langOnOpen), g_cfg.language);
        g_loading = TRUE;
        ApplyTheme(dlg);
        TranslateDialog(dlg);
        LoadIntoDialog(dlg);
        g_loading = FALSE;
        SetDirty(dlg, FALSE);
        Tabs_SetSel(GetDlgItem(dlg, IDC_TABS), g_page);
        ShowPage(dlg, g_page);
        PlaceDialog(dlg);

        g_filterDlg = dlg;
        g_filter = SetWindowsHookExW(WH_MSGFILTER, DialogFilter, NULL,
                                     GetCurrentThreadId());
        /* TRUE lets the dialog manager put the focus on the first control,
           which is the tab strip. */
        return TRUE;

    case WM_DESTROY:
        if (g_filter) {
            UnhookWindowsHookEx(g_filter);
            g_filter = NULL;
        }
        g_filterDlg = NULL;
        break;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
        if (!Theme_IsDark())
            break;
        return (INT_PTR)Theme_BgBrush();

    case WM_CTLCOLORSTATIC:
        if (!Theme_IsDark())
            break;
        SetTextColor((HDC)wp,
                     (HWND)lp == GetDlgItem(dlg, IDC_HDRSTATUS) ? Theme_DimText()
                                                                : Theme_Text());
        SetBkMode((HDC)wp, TRANSPARENT);
        return (INT_PTR)Theme_BgBrush();

    case WM_CTLCOLOREDIT:
        if (!Theme_IsDark())
            break;
        SetTextColor((HDC)wp, Theme_Text());
        SetBkColor((HDC)wp, Theme_Surface());
        return (INT_PTR)Theme_SurfaceBrush();

    case WM_SETTINGCHANGE:
        if (Theme_IsColorSchemeChange(msg, lp)) {
            Theme_SetDark(Theme_SystemPrefersDark());
            ApplyTheme(dlg);
            InvalidateRect(dlg, NULL, TRUE);
        }
        break;

    case WM_COMMAND:
        /* Anything the user edits on a page makes Apply available. The
           Browse buttons are left out on purpose: only the path they write
           into the edit box is a change, and that arrives as EN_CHANGE. */
        if (!g_loading && IsPageControl(LOWORD(wp)) &&
            LOWORD(wp) != IDC_BROWSE && LOWORD(wp) != IDC_VIDBROWSE &&
            (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == EN_CHANGE ||
             HIWORD(wp) == CBN_SELCHANGE))
            SetDirty(dlg, TRUE);

        switch (LOWORD(wp)) {
        case IDC_TABS:
            if (HIWORD(wp) == TABN_SELCHANGE)
                ShowPage(dlg, Tabs_GetSel((HWND)lp));
            return TRUE;
        case IDC_SAVEDISK:
        case IDC_JXR:
        case IDC_HOOKPRTSC:
        case IDC_RECWAV:
            UpdateEnabling(dlg);
            return TRUE;
        case IDC_RECAUDIO:
            if (HIWORD(wp) == CBN_SELCHANGE)
                UpdateEnabling(dlg);
            return TRUE;
        case IDC_LANGUAGE:
            /* Switch straight away rather than on OK: being asked to accept a
               change you cannot read yet is a poor way to choose a language. */
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT sel = SendDlgItemMessageW(dlg, IDC_LANGUAGE,
                                                  CB_GETCURSEL, 0, 0);
                LRESULT row = sel == CB_ERR ? CB_ERR
                    : SendDlgItemMessageW(dlg, IDC_LANGUAGE, CB_GETITEMDATA,
                                          (WPARAM)sel, 0);
                if (row != CB_ERR) {
                    StringCchCopyW(g_cfg.language, ARRAYSIZE(g_cfg.language),
                                   row > 0 && row <= g_langCount
                                       ? g_langCodes[row - 1] : L"auto");
                    Lang_Init(g_cfg.language);
                    g_loading = TRUE;
                    TranslateDialog(dlg);
                    FillAudioBox(dlg);
                    FillEncoderBox(dlg);
                    ShowHdrStatus(dlg);
                    /* The list itself holds "Automatic (...)", so it is text too. */
                    FillLanguageBox(dlg);
                    g_loading = FALSE;
                    InvalidateRect(dlg, NULL, TRUE);
                }
            }
            return TRUE;
        case IDC_BROWSE:
            BrowseForFolder(dlg, IDC_FOLDER, STR_UI_BROWSE_PICS);
            return TRUE;
        case IDC_VIDBROWSE:
            BrowseForFolder(dlg, IDC_VIDFOLDER, STR_UI_BROWSE_VIDS);
            return TRUE;
        case IDC_APPLY:
            /*
             * Store and activate, and stay open. What has been applied is now
             * the baseline Cancel goes back to - the language included, which
             * Cancel would otherwise switch back.
             */
            StoreFromDialog(dlg);
            StringCchCopyW(g_langOnOpen, ARRAYSIZE(g_langOnOpen), g_cfg.language);
            SetDirty(dlg, FALSE);
            return TRUE;
        case IDOK:
            SaveDialogPos(dlg);
            StoreFromDialog(dlg);
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            /*
             * Where the window sits is not one of the settings being edited,
             * so Cancel keeps it. g_cfg holds what was loaded or last applied -
             * StoreFromDialog has not run since - so writing it back is safe.
             */
            SaveDialogPos(dlg);
            if (_wcsicmp(g_cfg.language, g_langOnOpen) != 0) {
                StringCchCopyW(g_cfg.language, ARRAYSIZE(g_cfg.language),
                               g_langOnOpen);
                Lang_Init(g_cfg.language);
            }
            Settings_Save();
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

BOOL SettingsUI_Show(HWND owner)
{
    static BOOL open;       /* one at a time, or two dialogs fight over g_cfg */
    INT_PTR rc;

    if (open)
        return FALSE;
    if (!Tabs_Register())
        return FALSE;
    open = TRUE;
    rc = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETTINGS),
                         owner, SettingsProc, 0);
    open = FALSE;
    return rc == IDOK;
}
