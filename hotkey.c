/*
 * hotkey.c - the low-level keyboard hook behind Win+Shift+S and PrtScn.
 */

#include "nitshot.h"
#include "hotkey.h"
#include "settings.h"
#include "log.h"

#include <process.h>

/* Stamped on the keystrokes we inject ourselves, so the hook can skip exactly
   those. Filtering on LLKHF_INJECTED instead would be wrong: remote-desktop
   hosts (CRD, RDP shadowing, VMs) deliver every real keypress as injected, and
   the hotkey has to keep working in those sessions. */
#define BSNIP_INJECT_TAG  ((ULONG_PTR)0x42534E50)   /* 'BSNP' */

/*
 * The hook lives on a thread of its own that does nothing else.
 *
 * A low-level hook is called on the thread that installed it, and Windows
 * gives that thread LowLevelHooksTimeout to answer before passing the key on
 * without it - after which Win+Shift+S reaches the shell and Snip & Sketch
 * opens. On the UI thread that budget is shared with everything the UI does:
 * a grab of a cold duplication, a message box, a modal dialog's own work. Here
 * nothing can delay the answer but the hook itself.
 */
static HHOOK         g_hook;          /* owned by the hook thread only */
static HWND          g_notify;
static volatile LONG g_suppressed;
static HANDLE        g_thread;
static DWORD         g_threadId;

#define WM_HK_REINSTALL (WM_APP + 1)  /* posted to the hook thread */

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
                        if (InterlockedCompareExchange(&g_suppressed, 0, 0))
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
                    else if (InterlockedCompareExchange(&g_suppressed, 0, 0))
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

/* Runs on the hook thread. Install before unhooking, so a failed reinstall
   leaves the working hook in place instead of silently disarming the keys. */
static BOOL InstallHere(void)
{
    HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                    GetModuleHandleW(NULL), 0);
    if (!fresh)
        return g_hook != NULL;
    if (g_hook)
        UnhookWindowsHookEx(g_hook);
    g_hook = fresh;
    /*
     * The held-key table deliberately survives a reinstall. Reinstalling runs
     * every ten seconds as a safety net, and a key held across one of those
     * ticks would otherwise look released: its key-up would reach the
     * foreground app bare, and the next autorepeat would count as a fresh
     * press. Uninstall clears the table, because then nothing is swallowed.
     */
    return TRUE;
}

typedef struct {
    HANDLE ready;     /* set once the first install has been tried */
    BOOL   ok;
} HookStart;

static unsigned __stdcall HookThread(void *arg)
{
    HookStart *start = (HookStart *)arg;
    MSG        msg;

    /* It does almost nothing, but when it does, it is on every keystroke the
       user types; under load it should not queue behind the rest of us. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    /* A message queue has to exist before anyone may post to this thread. */
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    start->ok = InstallHere();
    SetEvent(start->ready);          /* 'start' is not ours after this */

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HK_REINSTALL)
            InstallHere();
    }

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    return 0;
}

BOOL Hotkey_Install(HWND notify)
{
    HookStart start;
    uintptr_t th;

    /* Set first: the hook can fire the moment it exists. */
    g_notify = notify;

    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_HK_REINSTALL, 0, 0);
        return TRUE;
    }

    start.ok    = FALSE;
    start.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!start.ready)
        return FALSE;

    th = _beginthreadex(NULL, 0, HookThread, &start, 0, (unsigned *)&g_threadId);
    if (!th) {
        CloseHandle(start.ready);
        return FALSE;
    }
    g_thread = (HANDLE)th;
    /* The caller reports a failed hook to the user, so the answer has to be
       known before returning. The thread is about to signal, never block. */
    WaitForSingleObject(start.ready, INFINITE);
    CloseHandle(start.ready);
    if (!start.ok)
        Hotkey_Uninstall();
    return start.ok;
}

void Hotkey_Uninstall(void)
{
    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread   = NULL;
        g_threadId = 0;
    }
    ZeroMemory(g_eaten, sizeof(g_eaten));
}

void Hotkey_Refresh(void)
{
    if (g_thread)
        PostThreadMessageW(g_threadId, WM_HK_REINSTALL, 0, 0);
}

void Hotkey_SetSuppressed(BOOL suppressed)
{
    InterlockedExchange(&g_suppressed, suppressed ? 1 : 0);
}
