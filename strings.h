/*
 * strings.h - every piece of text the user can read, in one list.
 *
 * The list is an X-macro so that the enum, the stable key names written in the
 * language files, and the built-in English text are all generated from it.
 * Keeping them in three hand-written tables would mean they eventually
 * disagree, and a key that no longer matches its string is exactly the kind of
 * fault nobody notices until it ships in the wrong language.
 *
 * Log lines and INI key names are deliberately not here: they are diagnostics
 * and file format, not user interface, and they stay English.
 *
 * Product vocabulary stays English in every language - Nitshot,
 * snip, HDR, PrtScn, Win+Shift+S, the file extensions and the codec names. A
 * German user looking for ".jxr" should find ".jxr".
 *
 * Placeholders are %1, %2, ... and are substituted positionally, so a
 * translation may put them in whatever order its grammar wants.
 */
#ifndef NITSHOT_STRINGS_H
#define NITSHOT_STRINGS_H

#define BSNIP_STRINGS(X)                                                      \
  /* ---- tray menu and tooltip ---- */                                       \
  X(MENU_SNIP,          "&New snip\tWin+Shift+S")                             \
  X(MENU_FULLSCREEN,    "Capture &whole screen")                              \
  X(MENU_OPENFOLDER,    "&Open screenshots folder")                           \
  X(MENU_AUTOSTART,     "Start with &Windows")                                \
  X(MENU_SETTINGS,      "&Settings...")                                       \
  X(MENU_ABOUT,         "&About")                                             \
  X(MENU_EXIT,          "E&xit")                                              \
  X(TRAY_TIP,           "Win+Shift+S or PrtScn to capture")                   \
                                                                              \
  /* ---- notifications after a snip ---- */                                  \
  X(TOAST_COPIED_SAVED, "Copied, and saved as\n%1")                           \
  X(TOAST_SAVED,        "Saved as\n%1")                                       \
  X(TOAST_COPIED,       "Copied to the clipboard.")                           \
  X(TOAST_NOTHING,      "Nothing was saved - check the settings.")            \
  X(TOAST_HDR,          "\nHDR")                                              \
  X(TOAST_HDR_SDR,      "\nHDR, with an SDR copy beside it")                  \
  X(TOAST_ENCODE,       "\nencode %1 ms")                                     \
                                                                              \
  /* ---- notifications after a recording ---- */                             \
  X(TOAST_RECORDED,     "Recorded %1 to\n%2")                                 \
  X(TOAST_RECORD_WAV,   "\nwith a lossless .wav beside it")                   \
  X(TOAST_RECORD_FAIL,  "The recording failed.\n%1")                          \
                                                                              \
  /* ---- things that went wrong ---- */                                      \
  X(MSG_RECORD_START,   "The recording could not be started.")                \
  X(MSG_CAPTURE_FAILED, "The screen could not be captured.")                  \
  X(MSG_SAVE_FAILED,    "The snip could not be saved.")                       \
  X(MSG_FOLDER_FAILED,  "The screenshots folder could not be created.")       \
  X(MSG_HOOK_FAILED,    "The keyboard hook could not be installed, so "       \
                        "Win+Shift+S and PrtScn will not be captured.\n\n"    \
                        "%1 will keep running; use the tray icon to capture.")\
  X(ERR_NO_ENCODER,     "No video encoder is available.")                     \
  X(ERR_NO_DISPLAY,     "No display could be captured.")                      \
  X(ERR_AREA_SMALL,     "The area is too small to record.")                   \
  X(ERR_HDR_SETUP,      "The HDR conversion could not be set up.")            \
  X(ERR_CAPTURE_BUF,    "The capture buffer could not be made.")              \
  X(ERR_FILE_CREATE,    "The video file could not be created.")               \
  X(ERR_ENC_FORMAT,     "The video encoder refused the recording format.")    \
  X(ERR_ENC_START,      "The encoder refused to start.")                      \
  X(ERR_FINALISE,       "The file could not be finalised.")                   \
                                                                              \
  /* ---- the About box; the name and version line is not translated ---- */  \
  X(ABOUT_BODY,         "A small native replacement for the Windows "         \
                        "Snipping Tool.\n\n"                                  \
                        "Win+Shift+S\topen the capture overlay\n"             \
                        "PrtScn\t\tthe same\n\n"                              \
                        "Screenshots go to:\n%1\n\n"                          \
                        "Copyright \xA9 2026 Rekow IT")                       \
                                                                              \
  /* ---- settings dialog: tabs ---- */                                       \
  /* Keyboard letters (&) only have to be unique within one tab, plus the     \
     three buttons, which are on every tab. */                                \
  X(DLG_CAPTION,        "Nitshot settings")                        \
  X(TAB_GENERAL,        "General")                                            \
  X(TAB_HDR,            "HDR")                                                \
  X(TAB_RECORD,         "Recording")                                          \
  X(TAB_HOTKEYS,        "Hotkeys")                                            \
  X(TAB_ADVANCED,       "Advanced")                                           \
                                                                              \
  /* ---- settings dialog: controls ---- */                                   \
  X(UI_COPYCLIP,        "Copy every snip to the &clipboard")                  \
  X(UI_SAVEDISK,        "Save &files")                                        \
  X(UI_FOLDER,          "&Pictures:")                                         \
  X(UI_BROWSE,          "&Browse...")                                         \
  X(UI_VIDFOLDER,       "V&ideos:")                                           \
  X(UI_VIDBROWSE,       "Brow&se...")                                         \
  X(UI_BROWSE_PICS,     "Where should snips be saved?")                       \
  X(UI_BROWSE_VIDS,     "Where should recordings be saved?")                  \
  X(UI_HOOKWSS,         "Take over &Win+Shift+S")                             \
  X(UI_HOOKPRTSC,       "Take over &PrtScn")                                  \
  X(UI_PRTSCFULL,       "PrtScn takes the whole screen without the &overlay") \
  X(UI_DIM,             "&Dim the desktop by")                                \
  X(UI_PERCENT,         "percent")                                            \
  X(UI_TOAST,           "Show a notification when a snip is sa&ved")          \
  X(UI_JXR,             "Save HDR snips as .&jxr")                            \
  X(UI_SIDECAR,         "...and a tone-mapped .png beside it")                \
  X(UI_ROLLOFF,         "Keep &highlight details in the 8-bit copy")          \
  X(UI_QUALITY,         "&Quality")                                           \
  X(UI_RECAUDIO,        "&Sound:")                                            \
  X(UI_RECFPS,          "F&rames per second:")                                \
  X(UI_RECENC,          "&Encoder:")                                          \
  X(UI_RECHDR,          "&Record in HDR when the display is in HDR")          \
  X(UI_RECWAV,          "Save the sound as a lossless .&wav beside the .mp4") \
  X(UI_RECMUX,          "...and build the .mp4's sound from it, keeping "     \
                        "only the .mp4")                                      \
  X(UI_LANGUAGE,        "&Language:")                                         \
  X(UI_AUTOSTART,       "Start with &Windows")                                \
  X(UI_DEBUGLOG,        "Diagnostic &log")                                    \
  X(UI_FORCEGDI,        "Always use &GDI capture")                            \
  X(UI_FORCESDR,        "Ignore HDR, capture 8-&bit")                         \
  X(UI_OK,              "OK")                                                 \
  X(UI_CANCEL,          "Cancel")                                             \
  X(UI_APPLY,           "&Apply")                                             \
                                                                              \
  /* ---- settings dialog: list entries and status ---- */                    \
  X(AUDIO_NONE,         "None")                                               \
  X(AUDIO_SYSTEM,       "System sound")                                       \
  X(AUDIO_MIC,          "Microphone")                                         \
  X(ENC_AUTO,           "Automatic (best available)")                         \
  X(ENC_H264,           "H.264, 8-bit")                                       \
  X(ENC_HEVC8,          "HEVC, 8-bit")                                        \
  X(ENC_HEVC10,         "HEVC, 10-bit HDR")                                   \
  X(ENC_SOFTWARE,       "software")                                           \
  X(ENC_HARDWARE,       " (hardware)")                                        \
  X(LANG_AUTO,          "Automatic (Windows setting)")                        \
  X(HDR_STATUS_ON,      "Advanced colour is on. SDR white is at %1 nits, "    \
                        "so snips are captured in floating point.")           \
  X(HDR_STATUS_OFF,     "No display has advanced colour on, so snips are "    \
                        "captured as ordinary 8-bit images.")

typedef enum {
#define BSNIP_ENUM(id, en) STR_##id,
    BSNIP_STRINGS(BSNIP_ENUM)
#undef BSNIP_ENUM
    STR_COUNT
} StringId;

#endif /* NITSHOT_STRINGS_H */
