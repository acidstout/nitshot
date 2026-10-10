/*
 * log.c - append-only UTF-8 diagnostic log, off by default.
 */

#include "nitshot.h"
#include "log.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdarg.h>

/* Past this the log is moved aside to Nitshot.old.log and started afresh, so a
   log left switched on for months cannot quietly fill the disk. */
#define LOG_MAX_BYTES  (4u * 1024u * 1024u)

static volatile LONG g_on;
static wchar_t       g_path[MAX_PATH];
static INIT_ONCE     g_pathOnce = INIT_ONCE_STATIC_INIT;

/* Once, for every thread: the hook thread, the recorder and the save workers
   all log, and a path being filled in by one while another reads it would
   send a line to a truncated file name. */
static BOOL CALLBACK InitPath(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    PWSTR appdata = NULL;

    UNREFERENCED_PARAMETER(once);
    UNREFERENCED_PARAMETER(param);
    UNREFERENCED_PARAMETER(ctx);

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        if (FAILED(StringCchPrintfW(g_path, MAX_PATH, L"%s\\%s\\Nitshot.log",
                                    appdata, APP_NAME_W)))
            g_path[0] = 0;
        CoTaskMemFree(appdata);
    }
    return TRUE;
}

static const wchar_t *LogPath(void)
{
    InitOnceExecuteOnce(&g_pathOnce, InitPath, NULL, NULL);
    return g_path;
}

void Log_Enable(BOOL on)
{
    InterlockedExchange(&g_on, on ? 1 : 0);
}

/* Moves a full log aside. Two threads may both see it full; MoveFileEx is
   atomic, so one wins and the other's move simply fails, which is fine. */
static void Rotate(void)
{
    wchar_t old[MAX_PATH];

    if (FAILED(StringCchCopyW(old, MAX_PATH, g_path)))
        return;
    PathRemoveExtensionW(old);
    if (FAILED(StringCchCatW(old, MAX_PATH, L".old.log")))
        return;
    MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING);
}

void Log_Printf(const wchar_t *fmt, ...)
{
    wchar_t    line[1024];
    wchar_t    whole[1152];
    /* UTF-8 needs up to three bytes for every UTF-16 unit; anything smaller
       made WideCharToMultiByte refuse a long line of non-Latin text, and the
       line silently went missing. */
    char       utf8[ARRAYSIZE(whole) * 3];
    SYSTEMTIME st;
    HANDLE     f;
    va_list    ap;
    int        bytes;
    DWORD      written;

    if (!InterlockedCompareExchange(&g_on, 0, 0) || !LogPath()[0])
        return;

    va_start(ap, fmt);
    StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);

    GetLocalTime(&st);
    StringCchPrintfW(whole, ARRAYSIZE(whole), L"%02u:%02u:%02u.%03u  %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    bytes = WideCharToMultiByte(CP_UTF8, 0, whole, -1, utf8, (int)sizeof(utf8),
                                NULL, NULL);
    if (bytes <= 1)
        return;
    bytes--;                      /* drop the terminating NUL */

    /* FILE_APPEND_DATA with sharing: a second instance or another thread
       writing at the same moment interleaves whole lines rather than losing
       them - each append is a single write. */
    f = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    WriteFile(f, utf8, (DWORD)bytes, &written, NULL);
    {
        LARGE_INTEGER size;
        BOOL          full = GetFileSizeEx(f, &size) &&
                             size.QuadPart >= (LONGLONG)LOG_MAX_BYTES;
        CloseHandle(f);
        if (full)
            Rotate();
    }
}
