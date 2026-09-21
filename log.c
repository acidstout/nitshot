/*
 * log.c - append-only UTF-8 diagnostic log, off by default.
 */

#include "nitshot.h"
#include "log.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdarg.h>

static BOOL    g_on;
static wchar_t g_path[MAX_PATH];

static const wchar_t *LogPath(void)
{
    PWSTR appdata = NULL;

    if (g_path[0])
        return g_path;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        StringCchPrintfW(g_path, MAX_PATH, L"%s\\%s\\Nitshot.log",
                         appdata, APP_NAME_W);
        CoTaskMemFree(appdata);
    }
    return g_path;
}

void Log_Enable(BOOL on)
{
    g_on = on;
}

void Log_Printf(const wchar_t *fmt, ...)
{
    wchar_t    line[1024];
    wchar_t    stamp[64];
    char       utf8[2048];
    SYSTEMTIME st;
    HANDLE     f;
    va_list    ap;
    int        bytes;
    DWORD      written;

    if (!g_on || !LogPath()[0])
        return;

    va_start(ap, fmt);
    StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);

    GetLocalTime(&st);
    StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%02u:%02u:%02u.%03u  ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    StringCchCatW(stamp, ARRAYSIZE(stamp), L"");

    {
        wchar_t whole[1152];
        StringCchPrintfW(whole, ARRAYSIZE(whole), L"%s%s\r\n", stamp, line);
        bytes = WideCharToMultiByte(CP_UTF8, 0, whole, -1, utf8, sizeof(utf8), NULL, NULL);
        if (bytes <= 1)
            return;
        bytes--;                      /* drop the terminating NUL */
    }

    /* FILE_APPEND_DATA with sharing: a second instance writing at the same
       moment interleaves lines rather than losing them. */
    f = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    WriteFile(f, utf8, (DWORD)bytes, &written, NULL);
    CloseHandle(f);
}
