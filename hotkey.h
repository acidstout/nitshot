/*
 * hotkey.h - global capture hotkeys.
 *
 * Win+Shift+S belongs to the shell, so RegisterHotKey cannot have it while
 * Snip & Sketch is installed. A WH_KEYBOARD_LL hook sees the keystroke before
 * the shell's own hook does (low-level hooks run most-recently-installed
 * first), which lets us take the chord over without uninstalling anything -
 * and give it straight back by quitting.
 */
#ifndef NITSHOT_HOTKEY_H
#define NITSHOT_HOTKEY_H

#include "nitshot.h"

/* Install the hook. Captured keys arrive at 'notify' as WM_BSNIP_TRIGGER with
   wParam = TriggerSource. Must be called from the thread that pumps messages. */
BOOL Hotkey_Install(HWND notify);

/* Tear the hook down. Safe to call when nothing is installed. */
void Hotkey_Uninstall(void);

/* Re-read g_cfg and reinstall. Also the periodic self-heal: Windows silently
   drops a low-level hook whose callback once ran past LowLevelHooksTimeout,
   and there is no way to ask whether that has happened. */
void Hotkey_Refresh(void);

/*
 * While the overlay owns the screen the hotkeys must do nothing - but they
 * must still be swallowed, or Win+Shift+S would fall through to the shell and
 * launch Snip & Sketch on top of our own overlay.
 */
void Hotkey_SetSuppressed(BOOL suppressed);

#endif /* NITSHOT_HOTKEY_H */
