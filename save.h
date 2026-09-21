/*
 * save.h - where a snip ends up: the clipboard and a PNG on disk.
 *
 * Both destinations go through one call, because they share the encoded PNG.
 * Encoding a 4K screen twice cost more than the capture itself.
 *
 * Phase 4 adds the HDR side (JPEG XR plus a tone-mapped sidecar); the
 * clipboard stays SDR either way, because Windows has no HDR clipboard format.
 */
#ifndef NITSHOT_SAVE_H
#define NITSHOT_SAVE_H

#include "nitshot.h"
#include "capture.h"

typedef struct {
    BOOL    clipboardOk;          /* the image reached the clipboard         */
    BOOL    diskOk;               /* 'path' was written                      */
    BOOL    sidecarOk;            /* the tone-mapped .png beside a .jxr      */
    BOOL    hdr;                  /* the capture was floating point          */
    wchar_t path[MAX_PATH];       /* full path of the file, if diskOk        */
    long    encodeMs;             /* encoding alone, for the diagnostics     */
} SaveResult;

/* Creates the WIC factory. Requires COM to be initialised on this thread. */
BOOL Save_Init(void);
void Save_Shutdown(void);

/*
 * Publishes and/or stores the image, honouring g_cfg.copyToClipboard and
 * g_cfg.saveToDisk. The clipboard gets CF_DIB, CF_DIBV5 and the registered
 * "PNG" format, so both old Win32 apps and modern ones find something they
 * understand. The file is "Screenshot YYYY-MM-DD HHMMSS.png" in the configured
 * folder, with " (2)", " (3)" ... if that name is taken.
 *
 * Returns TRUE if at least one destination succeeded; 'res' says which.
 */
BOOL Save_Snip(HWND owner, const SnipImage *img, SaveResult *res);

/*
 * The same work on a worker thread. This is not an optimisation, it is a
 * correctness fix: a WH_KEYBOARD_LL callback is dispatched on the thread that
 * installed the hook, so while the message loop is busy the hook cannot run.
 * Windows then times it out and passes the keystroke on - which for us means
 * Win+Shift+S falling through to the shell and Snip & Sketch opening. A 4K HDR
 * snip spends seconds in the encoder, which is long enough for that to happen.
 *
 * Takes ownership of 'img' and zeroes the caller's struct. When the save
 * finishes, 'doneMsg' is posted to 'notify' with a heap-allocated SaveResult
 * as lParam, which the receiver must free with Save_FreeResult().
 * Returns FALSE if the thread could not be started, in which case 'img' is
 * freed and nothing is posted.
 */
BOOL Save_SnipAsync(HWND notify, UINT doneMsg, SnipImage *img);

void Save_FreeResult(SaveResult *res);

/* Write one SDR image straight to a path as PNG, ignoring the settings. Used
   by the overlay's debug back-buffer dump. */
BOOL Save_ImageToPath(const SnipImage *img, const wchar_t *path);

#endif /* NITSHOT_SAVE_H */
