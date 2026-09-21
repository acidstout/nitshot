/*
 * hotkey.c - the low-level keyboard hook behind Win+Shift+S and PrtScn.
 */

#include "nitshot.h"
#include "hotkey.h"
#include "settings.h"
#include "log.h"

/* Stamped on the keystrokes we inject ourselves, so the hook can skip exactly
   those. Filtering on LLKHF_INJECTED instead would be wrong: remote-desktop
   hosts (CRD, RDP shadowing, VMs) deliver every real keypress as injected, and
   the hotkey has to keep working in those sessions. */
#define BSNIP_INJECT_TAG  ((ULONG_PTR)0x42534E50)   /* 'BSNP' */

static HHOOK g_hook;
static HWND  g_notify;
static BOOL  g_suppressed;

/* Key-downs we swallowed, so the matching key-up can be swallowed too and no
   application is left thinking the key is still held. Four slots is more than
   the two keys we ever eat. */
static BYTE  g_eaten[4];

static BOOL Down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* Records a swallowed key-down. FALSE means the key was already held, i.e.
   this is auto-repeat - holding the chord must not fire capture after capture. */
static BOOL RememberEaten(BYTE vk)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(g_eaten); i++) {
        if (g_eaten[i] == vk)
            return FALSE;
    }
    for (i = 0; i < (int)ARRAYSIZE(g_eaten); i++) {
        if (g_eaten[i] == 0) {
            g_eaten[i] = vk;
            return TRUE;
        }
    }
    g_eaten[0] = vk;   /* full: overwrite the oldest slot */
    return TRUE;
}

static BOOL ForgetEaten(BYTE vk)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(g_eaten); i++) {
        if (g_eaten[i] == vk) {
            g_eaten[i] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Swallowing the 'S' leaves the shell believing the Windows key was tapped on
 * its own, so releasing it would open the Start menu. Feeding it a reserved
 * virtual key marks the chord as consumed. VK_NONAME is reserved precisely for
 * this kind of filler and is bound to nothing.
 */
static void ConsumeWinChord(void)
{
    INPUT in[2];

    ZeroMemory(in, sizeof(in));
    in[0].type              = INPUT_KEYBOARD;
    in[0].ki.wVk            = VK_NONAME;
    in[0].ki.dwExtraInfo    = BSNIP_INJECT_TAG;
    in[1].type              = INPUT_KEYBOARD;
    in[1].ki.wVk            = VK_NONAME;
    in[1].ki.dwFlags        = KEYEVENTF_KEYUP;
    in[1].ki.dwExtraInfo    = BSNIP_INJECT_TAG;
    SendInput(2, in, sizeof(INPUT));
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_notify) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;

        /* Skip only our own filler keystroke; see BSNIP_INJECT_TAG. */
        if (k->dwExtraInfo != BSNIP_INJECT_TAG) {
            BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            BOOL up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);

            if (down) {
                if (g_cfg.hookWinShiftS && k->vkCode == 'S' &&
                    (Down(VK_LWIN) || Down(VK_RWIN)) && Down(VK_SHIFT) &&
                    !Down(VK_CONTROL) && !Down(VK_MENU)) {
                    if (RememberEaten((BYTE)k->vkCode)) {
                        ConsumeWinChord();
                        if (g_suppressed)
                            Log_Printf(L"hotkey: Win+Shift+S eaten (overlay up)");
                        else
                            PostMessageW(g_notify, WM_BSNIP_TRIGGER,
                                         TRIGGER_HOTKEY_SNIP, 0);
                    } else {
                        Log_Printf(L"hotkey: Win+Shift+S eaten (still held)");
                    }
                    return 1;
                }
                if (g_cfg.hookPrintScreen && k->vkCode == VK_SNAPSHOT &&
                    !Down(VK_MENU)) {   /* Alt+PrtScn stays the OS's window grab */
                    if (!RememberEaten((BYTE)k->vkCode))
                        Log_Printf(L"hotkey: PrtScn eaten (still held)");
                    else if (g_suppressed)
                        Log_Printf(L"hotkey: PrtScn eaten (overlay up)");
                    else
                        PostMessageW(g_notify, WM_BSNIP_TRIGGER, TRIGGER_PRINTSCREEN, 0);
                    return 1;
                }
            } else if (up && ForgetEaten((BYTE)k->vkCode)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

BOOL Hotkey_Install(HWND notify)
{
    HHOOK old = g_hook;
    HHOOK fresh;

    /* Set first: the new hook can fire the moment it exists. */
    g_notify = notify;

    fresh = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(NULL), 0);
    if (!fresh) {
        /* Install before unhooking, so a failed reinstall leaves the working
           hook in place instead of silently disarming the hotkeys. */
        return old != NULL;
    }

    g_hook = fresh;
    if (old)
        UnhookWindowsHookEx(old);
    ZeroMemory(g_eaten, sizeof(g_eaten));
    return TRUE;
}

void Hotkey_Uninstall(void)
{
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    ZeroMemory(g_eaten, sizeof(g_eaten));
}

void Hotkey_Refresh(void)
{
    if (g_notify)
        Hotkey_Install(g_notify);
}

void Hotkey_SetSuppressed(BOOL suppressed)
{
    g_suppressed = suppressed;
}
