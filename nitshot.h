/*
 * nitshot.h - names and types shared across the whole program.
 */
#ifndef NITSHOT_H
#define NITSHOT_H

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#define APP_NAME_W      L"Nitshot"
#define APP_TITLE_W     L"Nitshot"
#define APP_CLASS_W     L"Nitshot.Host"
#define APP_MUTEX_W     L"Local\\Nitshot.SingleInstance"

/* What the user is capturing. The order matches the toolbar, left to right. */
typedef enum {
    SNIP_RECT = 0,   /* drag a rectangle                       */
    SNIP_FREE,       /* draw a closed freehand shape           */
    SNIP_WINDOW,     /* pick a window under the cursor         */
    SNIP_FULLSCREEN, /* the monitor under the cursor, or all   */
    SNIP_MODE_COUNT
} SnipMode;

/* Where a capture request came from - only used for diagnostics and for
   deciding which mode a bare PrtScn should start in. */
typedef enum {
    TRIGGER_HOTKEY_SNIP = 0, /* Win+Shift+S            */
    TRIGGER_PRINTSCREEN,     /* PrtScn                 */
    TRIGGER_TRAY,            /* tray menu / double click */
    TRIGGER_SECOND_INSTANCE, /* another copy was started */
    TRIGGER_FULLSCREEN       /* the tray's "whole screen", no overlay */
} TriggerSource;

/* Posted to the host window when a hotkey fires.
   wParam = TriggerSource, lParam = unused. */
#define WM_BSNIP_TRIGGER  (WM_APP + 2)

/* Posted by the save worker when it is done.
   lParam = SaveResult *, which the receiver frees. */
#define WM_BSNIP_SAVED    (WM_APP + 3)

/* Posted by the recording worker when it is done.
   lParam = RecordResult *, which the receiver frees. */
#define WM_BSNIP_RECORDED (WM_APP + 4)

/* Posted by the record bar's stop button. */
#define WM_BSNIP_STOPREC  (WM_APP + 5)

#endif /* NITSHOT_H */
