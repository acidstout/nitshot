/*
 * settings.c - INI load/save, save folder resolution, autostart.
 */

#include "nitshot.h"
#include "settings.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

Settings g_cfg;

static wchar_t g_iniPath[MAX_PATH];

#define SEC L"Nitshot"

/*
 * The program was called BetterSnippingTool until 0.8. Its data folder, INI
 * section and autostart value carried that name, and an upgrade should not
 * greet anyone with factory settings, so the old ones are taken over once.
 */
#define LEGACY_NAME_W L"BetterSnippingTool"

/* %APPDATA%\Nitshot\settings.ini, creating the directory. */
const wchar_t *Settings_IniPath(void)
{
    PWSTR appdata = NULL;

    if (g_iniPath[0])
        return g_iniPath;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        wchar_t dir[MAX_PATH], old[MAX_PATH];
        if (SUCCEEDED(StringCchPrintfW(dir, MAX_PATH, L"%s\\%s", appdata, APP_NAME_W))) {
            /* Rename the whole folder: it also holds the encoder cache, the
               log and any user translations. Only ever into a folder that
               does not exist yet, so nothing current can be overwritten. */
            if (SUCCEEDED(StringCchPrintfW(old, MAX_PATH, L"%s\\%s", appdata, LEGACY_NAME_W)) &&
                GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES &&
                PathIsDirectoryW(old))
                MoveFileExW(old, dir, 0);
            SHCreateDirectoryExW(NULL, dir, NULL);
            StringCchPrintfW(g_iniPath, MAX_PATH, L"%s\\settings.ini", dir);
        }
        CoTaskMemFree(appdata);
    }

    /* Falling back next to the exe keeps a broken profile from losing settings
       entirely; it is also what a portable copy on a stick wants. */
    if (!g_iniPath[0]) {
        GetModuleFileNameW(NULL, g_iniPath, MAX_PATH);
        PathRemoveFileSpecW(g_iniPath);
        StringCchCatW(g_iniPath, MAX_PATH, L"\\settings.ini");
    }
    return g_iniPath;
}

static BOOL GetBool(const wchar_t *key, BOOL def)
{
    return GetPrivateProfileIntW(SEC, key, def ? 1 : 0, Settings_IniPath()) != 0;
}

static void PutBool(const wchar_t *key, BOOL v)
{
    WritePrivateProfileStringW(SEC, key, v ? L"1" : L"0", Settings_IniPath());
}

static void PutInt(const wchar_t *key, int v)
{
    wchar_t buf[16];
    StringCchPrintfW(buf, 16, L"%d", v);
    WritePrivateProfileStringW(SEC, key, buf, Settings_IniPath());
}

/*
 * GetPrivateProfileInt is documented to return zero for any value below zero,
 * which is fine for percentages and useless for window coordinates: a monitor
 * to the left of the primary has negative x. Read those as text.
 */
static int GetSignedInt(const wchar_t *key, int def)
{
    wchar_t  buf[32], *end = NULL;
    long     v;

    GetPrivateProfileStringW(SEC, key, L"", buf, ARRAYSIZE(buf),
                             Settings_IniPath());
    if (!buf[0])
        return def;
    v = wcstol(buf, &end, 10);
    if (end == buf)
        return def;
    return (int)v;
}

#define RUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* The settings keep their values across the rename: the moved INI still has
   them under the old section name, and autostart is re-registered under the
   new value name, pointing at this exe rather than the old one. */
static void MigrateLegacy(const wchar_t *ini)
{
    static wchar_t buf[32767];      /* the documented maximum for a section */
    wchar_t        probe[4];
    HKEY           key;

    if (GetPrivateProfileSectionW(LEGACY_NAME_W, buf, ARRAYSIZE(buf), ini) > 0 &&
        GetPrivateProfileSectionW(SEC, probe, ARRAYSIZE(probe), ini) == 0) {
        WritePrivateProfileSectionW(SEC, buf, ini);
        WritePrivateProfileStringW(LEGACY_NAME_W, NULL, NULL, ini);
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                      &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, LEGACY_NAME_W, NULL, NULL, NULL, NULL) == ERROR_SUCCESS &&
            RegDeleteValueW(key, LEGACY_NAME_W) == ERROR_SUCCESS)
            Settings_SetAutostart(TRUE);
        RegCloseKey(key);
    }
}

void Settings_Load(void)
{
    const wchar_t *ini = Settings_IniPath();
    int mode;

    MigrateLegacy(ini);

    g_cfg.copyToClipboard = GetBool(L"CopyToClipboard", TRUE);
    g_cfg.saveToDisk      = GetBool(L"SaveToDisk",      TRUE);
    GetPrivateProfileStringW(SEC, L"SaveFolder", L"", g_cfg.saveFolder, MAX_PATH, ini);
    GetPrivateProfileStringW(SEC, L"VideoFolder", L"", g_cfg.videoFolder, MAX_PATH, ini);

    g_cfg.hookWinShiftS   = GetBool(L"HookWinShiftS",   TRUE);
    g_cfg.hookPrintScreen = GetBool(L"HookPrintScreen", TRUE);
    g_cfg.prtScnFullscreen= GetBool(L"PrtScnFullscreen", FALSE);

    mode = GetPrivateProfileIntW(SEC, L"DefaultMode", SNIP_RECT, ini);
    if (mode < 0 || mode >= SNIP_MODE_COUNT)
        mode = SNIP_RECT;
    g_cfg.defaultMode = (SnipMode)mode;

    g_cfg.dimPercent = GetPrivateProfileIntW(SEC, L"DimPercent", 40, ini);
    if (g_cfg.dimPercent < 0)   g_cfg.dimPercent = 0;
    if (g_cfg.dimPercent > 90)  g_cfg.dimPercent = 90;

    g_cfg.showToast     = GetBool(L"ShowToast",     TRUE);
    g_cfg.hdrSaveJxr    = GetBool(L"HdrSaveJxr",    TRUE);
    g_cfg.hdrSdrSidecar = GetBool(L"HdrSdrSidecar", TRUE);
    g_cfg.hdrSdrRollOff = GetBool(L"HdrSdrRollOff", FALSE);

    g_cfg.hdrJxrQuality = GetPrivateProfileIntW(SEC, L"HdrJxrQuality", 90, ini);
    if (g_cfg.hdrJxrQuality < 1)   g_cfg.hdrJxrQuality = 1;
    if (g_cfg.hdrJxrQuality > 100) g_cfg.hdrJxrQuality = 100;
    g_cfg.forceGdi      = GetBool(L"ForceGdi",      FALSE);
    g_cfg.recordAudio = GetPrivateProfileIntW(SEC, L"RecordAudio", 1, ini);
    if (g_cfg.recordAudio < 0 || g_cfg.recordAudio > 2)
        g_cfg.recordAudio = 1;
    g_cfg.recordFps = GetPrivateProfileIntW(SEC, L"RecordFps", 30, ini);
    if (g_cfg.recordFps < 5)  g_cfg.recordFps = 5;
    if (g_cfg.recordFps > 60) g_cfg.recordFps = 60;

    g_cfg.recordRawAudio = GetBool(L"RecordRawAudio", TRUE);

    GetPrivateProfileStringW(SEC, L"RecordEncoder", L"auto", g_cfg.recordEncoder,
                             ARRAYSIZE(g_cfg.recordEncoder), ini);
    g_cfg.recordHdr = GetBool(L"RecordHdr", TRUE);
    g_cfg.recordWavSidecar = GetBool(L"RecordWavSidecar", FALSE);
    g_cfg.recordMuxFromWav = GetBool(L"RecordMuxFromWav", FALSE);

    GetPrivateProfileStringW(SEC, L"Language", L"auto", g_cfg.language,
                             ARRAYSIZE(g_cfg.language), ini);

    g_cfg.settingsX = GetSignedInt(L"SettingsX", SETTINGS_POS_UNSET);
    g_cfg.settingsY = GetSignedInt(L"SettingsY", SETTINGS_POS_UNSET);

    g_cfg.forceSdr      = GetBool(L"ForceSdr",      FALSE);
    g_cfg.debugLog      = GetBool(L"DebugLog",      FALSE);
}

void Settings_Save(void)
{
    PutBool(L"CopyToClipboard", g_cfg.copyToClipboard);
    PutBool(L"SaveToDisk",      g_cfg.saveToDisk);
    WritePrivateProfileStringW(SEC, L"SaveFolder", g_cfg.saveFolder, Settings_IniPath());
    WritePrivateProfileStringW(SEC, L"VideoFolder", g_cfg.videoFolder, Settings_IniPath());

    PutBool(L"HookWinShiftS",    g_cfg.hookWinShiftS);
    PutBool(L"HookPrintScreen",  g_cfg.hookPrintScreen);
    PutBool(L"PrtScnFullscreen", g_cfg.prtScnFullscreen);
    PutInt (L"DefaultMode",      (int)g_cfg.defaultMode);

    PutInt (L"DimPercent",   g_cfg.dimPercent);
    PutBool(L"ShowToast",    g_cfg.showToast);
    PutBool(L"HdrSaveJxr",   g_cfg.hdrSaveJxr);
    PutBool(L"HdrSdrSidecar",g_cfg.hdrSdrSidecar);
    PutBool(L"HdrSdrRollOff",g_cfg.hdrSdrRollOff);
    PutInt (L"HdrJxrQuality", g_cfg.hdrJxrQuality);
    PutBool(L"ForceGdi",     g_cfg.forceGdi);
    PutInt (L"RecordAudio",  g_cfg.recordAudio);
    PutInt (L"RecordFps",    g_cfg.recordFps);
    PutBool(L"RecordRawAudio", g_cfg.recordRawAudio);
    WritePrivateProfileStringW(SEC, L"RecordEncoder", g_cfg.recordEncoder,
                               Settings_IniPath());
    PutBool(L"RecordHdr", g_cfg.recordHdr);
    PutBool(L"RecordWavSidecar", g_cfg.recordWavSidecar);
    PutBool(L"RecordMuxFromWav", g_cfg.recordMuxFromWav);
    WritePrivateProfileStringW(SEC, L"Language", g_cfg.language,
                               Settings_IniPath());
    PutInt (L"SettingsX", g_cfg.settingsX);
    PutInt (L"SettingsY", g_cfg.settingsY);
    PutBool(L"ForceSdr",     g_cfg.forceSdr);
    PutBool(L"DebugLog",     g_cfg.debugLog);
}

/* 'custom' if set, otherwise <known folder>\Nitshot; created. */
static BOOL ResolveFolder(const wchar_t *custom, REFKNOWNFOLDERID known,
                         wchar_t *out, size_t cch)
{
    PWSTR base = NULL;

    if (!out || cch == 0)
        return FALSE;
    out[0] = 0;

    if (custom && custom[0]) {
        if (FAILED(StringCchCopyW(out, cch, custom)))
            return FALSE;
    } else {
        if (FAILED(SHGetKnownFolderPath(known, 0, NULL, &base)))
            return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\%s", base, APP_NAME_W))) {
            CoTaskMemFree(base);
            return FALSE;
        }
        CoTaskMemFree(base);
    }

    /* ERROR_ALREADY_EXISTS is the common case and is not a failure. */
    {
        int rc = SHCreateDirectoryExW(NULL, out, NULL);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
            return FALSE;
    }
    return TRUE;
}

BOOL Settings_ResolveSaveFolder(wchar_t *out, size_t cch)
{
    return ResolveFolder(g_cfg.saveFolder, &FOLDERID_Pictures, out, cch);
}

BOOL Settings_DefaultFolder(BOOL videos, wchar_t *out, size_t cch)
{
    return ResolveFolder(NULL, videos ? &FOLDERID_Videos : &FOLDERID_Pictures,
                         out, cch);
}

BOOL Settings_ResolveVideoFolder(wchar_t *out, size_t cch)
{
    /* Videos, not Pictures: a recording is not a screenshot, and every other
       tool on the system puts captures there. */
    return ResolveFolder(g_cfg.videoFolder, &FOLDERID_Videos, out, cch);
}


BOOL Settings_GetAutostart(void)
{
    HKEY  key;
    BOOL  found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    found = RegQueryValueExW(key, APP_NAME_W, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

BOOL Settings_SetAutostart(BOOL enable)
{
    HKEY    key;
    LSTATUS rc;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return FALSE;

    if (enable) {
        wchar_t exe[MAX_PATH], quoted[MAX_PATH + 2];
        DWORD   n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            RegCloseKey(key);
            return FALSE;
        }
        StringCchPrintfW(quoted, MAX_PATH + 2, L"\"%s\"", exe);
        rc = RegSetValueExW(key, APP_NAME_W, 0, REG_SZ, (const BYTE *)quoted,
                            (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, APP_NAME_W);
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}
