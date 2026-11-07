/*
 * capture.h - screen capture and the full-screen selection overlay.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <windows.h>
#include "imgenc.h"

/* Copies the whole desktop (all monitors where supported) into img.
 * *origin receives the screen coordinates of the image's top-left pixel.
 * Also snapshots the visible top-level windows for window selection. */
int  CaptureScreen(LS_IMAGE *img, POINT *origin);

/*
 * Shows the frozen screenshot full screen, dimmed, and lets the user drag a
 * rectangle or click a window (Ctrl+click picks the control under the cursor).
 * Runs a modal message loop. Returns 1 and the selection in image coordinates
 * (left, top, right, bottom exclusive), or 0 if the user pressed Esc or
 * right-clicked.
 */
int  SelectRegion(HINSTANCE inst, const LS_IMAGE *img, POINT origin, RECT *sel);

/* Puts a 24-bit image on the clipboard as CF_DIB. */
int  CopyImageToClipboard(HWND owner, const LS_IMAGE *img);

#endif
