#ifndef THEME_H
#define THEME_H

#include <windows.h>

/*
 * Dark-mode support for the classic Win32 UI.
 *
 * Windows never themed the common controls for dark mode through a public API,
 * so this uses the private uxtheme.dll ordinals (Windows 10 1809 / build 17763
 * and later) and paints the parts that no theme fixes.
 *
 * Derived from the same module in the SystemInfo project, trimmed to the
 * controls this app uses (no tab control, no status bar) and extended with
 * check boxes, group boxes and edit controls.
 */

/* Load the private uxtheme entry points. Call once, before creating windows.
   Returns TRUE if this Windows version can do dark mode at all. */
BOOL Theme_Init(void);

/* TRUE if dark mode is available on this Windows version. */
BOOL Theme_Supported(void);

/* The current Windows-wide "Apps mode" setting (TRUE = dark). */
BOOL Theme_SystemPrefersDark(void);

/* Current app mode. */
BOOL Theme_IsDark(void);

/* Switch the app mode. Also updates the process-wide preferred app mode so
   newly created controls come up in the right colours. */
void Theme_SetDark(BOOL dark);

/* Per-window setup. Each of these is idempotent and may be called again after
   a mode switch; the subclasses check the mode at paint time. */
void Theme_ApplyToMainWindow(HWND hwnd);   /* title bar + background        */
void Theme_ApplyToListView(HWND hwnd);     /* colours, border, header       */
void Theme_ApplyToButton(HWND hwnd);       /* owner-painted push button     */
void Theme_ApplyToCheckBox(HWND hwnd);     /* dark glyph, text via parent   */
void Theme_ApplyToGroupBox(HWND hwnd);     /* owner-painted frame + caption */
void Theme_ApplyToEdit(HWND hwnd);         /* dark scrollbars, no 3D edge   */

/* Colours for the active mode. */
COLORREF Theme_Bg(void);            /* dialog / window background      */
COLORREF Theme_Surface(void);       /* list and edit background        */
COLORREF Theme_Text(void);          /* normal text                     */
COLORREF Theme_DimText(void);       /* secondary text                  */
COLORREF Theme_Line(void);          /* separators and frames           */
COLORREF Theme_Warn(void);          /* "border detected"               */
COLORREF Theme_Ok(void);            /* "clean"                         */
COLORREF Theme_RowHighlight(void);  /* list row with the capture DLL   */
HBRUSH   Theme_BgBrush(void);       /* brush for Theme_Bg()            */
HBRUSH   Theme_SurfaceBrush(void);  /* brush for Theme_Surface()       */

/* TRUE for the WM_SETTINGCHANGE that announces a light/dark switch. */
BOOL Theme_IsColorSchemeChange(UINT msg, LPARAM lp);

#endif /* THEME_H */
