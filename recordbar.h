/*
 * recordbar.h - what is on screen while a recording runs.
 *
 * Two windows: a small bar with the elapsed time and a stop button, and a red
 * frame around the area being recorded. The frame matters more than it looks -
 * without it there is nothing to say which part of the screen is being taken,
 * and nothing to distinguish "recording" from "not recording" at a glance.
 */
#ifndef NITSHOT_RECORDBAR_H
#define NITSHOT_RECORDBAR_H

#include "nitshot.h"

/* 'stopMsg' is posted to 'owner' when the stop button is pressed. */
BOOL RecordBar_Show(HWND owner, const RECT *region, UINT stopMsg);
void RecordBar_Hide(void);

#endif /* NITSHOT_RECORDBAR_H */
