/*
 * settings.h - the user-visible options, backed by a plain INI.
 *
 * The file lives at %APPDATA%\Nitshot\settings.ini so the exe stays
 * portable-looking but never needs write access next to itself. Every field
 * has a sane default, so a missing or half-written file is not an error.
 */
#ifndef NITSHOT_SETTINGS_H
#define NITSHOT_SETTINGS_H

#include "nitshot.h"

typedef struct {
    BOOL     copyToClipboard;   /* put every snip on the clipboard        */
    BOOL     saveToDisk;        /* also write a file                      */
    wchar_t  saveFolder[MAX_PATH]; /* empty = Pictures\Nitshot */
    wchar_t  videoFolder[MAX_PATH];/* empty = Videos\Nitshot   */

    BOOL     hookWinShiftS;     /* take over Win+Shift+S                  */
    BOOL     hookPrintScreen;   /* take over PrtScn                       */
    SnipMode defaultMode;       /* mode the overlay opens in              */
    BOOL     prtScnFullscreen;  /* PrtScn grabs everything, no overlay    */

    int      dimPercent;        /* how far the overlay dims the desktop   */
    BOOL     showToast;         /* tray balloon naming the saved file     */

    BOOL     hdrSaveJxr;        /* HDR snips go to .jxr                   */
    BOOL     hdrSdrSidecar;     /* ...plus a tone-mapped .png next to it   */
    BOOL     hdrSdrRollOff;     /* keep highlight detail in the 8-bit copy,
                                   at the cost of SDR white landing near 233 */
    int      hdrJxrQuality;     /* 1..100; 100 means lossless, which on busy
                                   content costs around 19 s for a 4K frame  */

    int      recordAudio;       /* AudioSource: 0 none, 1 system, 2 mic    */
    int      recordFps;
    BOOL     recordRawAudio;    /* bypass the endpoint's effects when capturing */
    wchar_t  recordEncoder[16]; /* EncoderId token: auto / h264 / hevc8 / hevc10 */
    BOOL     recordHdr;         /* capture 10-bit HDR when the display is in it */
    BOOL     recordWavSidecar;  /* also write the untouched PCM beside the MP4 */
    BOOL     recordMuxFromWav;  /* ...and build the MP4's AAC from that WAV
                                   afterwards, so nothing encodes audio live */

    /* Where the settings dialog was last left. SETTINGS_POS_UNSET means it has
       never been moved, and it keeps being centred. */
    int      settingsX, settingsY;

    wchar_t  language[16];      /* "auto", or a tag with a langs\<tag>.ini */

    BOOL     forceSdr;          /* ignore advanced colour, capture 8-bit  */
    BOOL     debugLog;          /* append diagnostics to Nitshot.log */
    BOOL     forceGdi;          /* skip duplication - diagnostics, and an
                                   escape hatch on displays where it misbehaves */
} Settings;

/* Far outside any real desktop, so it cannot collide with a saved position. */
#define SETTINGS_POS_UNSET  (-1000000)

/* The one live instance. Valid after Settings_Load(). */
extern Settings g_cfg;

void Settings_Load(void);
void Settings_Save(void);

/* Full path of the INI. Other modules keep their own sections in it - the
   encoder probe cache is one - rather than inventing a second file. */
const wchar_t *Settings_IniPath(void);

/* Full path of the folder snips go into, creating it if needed.
   Returns FALSE only if the folder could not be created. */
BOOL Settings_ResolveSaveFolder(wchar_t *out, size_t cch);

/* The same for recordings. Unlike snips, a recording is always written to
   disk, so this does not depend on saveToDisk. */
BOOL Settings_ResolveVideoFolder(wchar_t *out, size_t cch);

/* The built-in default for either folder, ignoring any custom one - what a
   setting is compared against to decide whether it is really a custom path. */
BOOL Settings_DefaultFolder(BOOL videos, wchar_t *out, size_t cch);

/* HKCU ..\CurrentVersion\Run entry. */
BOOL Settings_GetAutostart(void);
BOOL Settings_SetAutostart(BOOL enable);

#endif /* NITSHOT_SETTINGS_H */
