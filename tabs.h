/*
 * tabs.h - a small tab strip for the settings dialog.
 *
 * Not the common tab control, for two reasons. Its themed pane is painted in
 * a lighter colour than the dialog, so every checkbox and label placed on it
 * shows a grey box around its text unless each page is turned into a separate
 * child dialog. And it ignores dark mode entirely, which leaves a white strip
 * across an otherwise dark window. This draws only the row of labels, in the
 * colours theme.c already uses, and leaves the page area to the dialog.
 *
 * It is a normal child control: WS_TABSTOP, arrow keys move between tabs, and
 * a change of tab is reported to the parent as WM_COMMAND with TABN_SELCHANGE
 * in the high word.
 */
#ifndef NITSHOT_TABS_H
#define NITSHOT_TABS_H

#include "nitshot.h"

#define TABS_CLASS      L"BsnipTabs"
#define TABN_SELCHANGE  1
#define TABS_MAX        8

/* Once, before the dialog that uses the class is created. */
BOOL Tabs_Register(void);

/* Replaces the labels; the strings are copied. */
void Tabs_SetLabels(HWND tabs, const wchar_t *const *labels, int count);

int  Tabs_GetSel(HWND tabs);

/* Changes the selection and notifies the parent, as a click would. */
void Tabs_SetSel(HWND tabs, int index);

#endif /* NITSHOT_TABS_H */
