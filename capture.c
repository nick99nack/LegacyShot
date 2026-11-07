/*
 * capture.c - screen capture and the Lightshot-style selection overlay.
 *
 * The desktop is copied once into a 24-bit DIB, then shown full screen and
 * dimmed in a topmost window. The selected area is drawn undimmed with a
 * dotted border and its size. Hovering highlights the window under the cursor
 * (from a snapshot of the top-level windows taken with the screenshot); a
 * click without dragging selects it. Ctrl picks the control under the cursor.
 *
 * Win32 only, ANSI, VC++ 6.0 headers. Works on Windows 95 / NT 4 and later;
 * multi-monitor and DWM-aware window bounds are used when the OS has them.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "capture.h"

/* Not in the VC++ 6.0 headers (they are 0 / ignored on older systems). */
#ifndef SM_XVIRTUALSCREEN
#define SM_XVIRTUALSCREEN  76
#define SM_YVIRTUALSCREEN  77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#endif
#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif
#ifndef CWP_SKIPINVISIBLE
#define CWP_SKIPINVISIBLE   0x0001
#define CWP_SKIPTRANSPARENT 0x0004
#endif
#ifndef WS_EX_LAYERED
#define WS_EX_LAYERED 0x00080000
#endif
#define DWMWA_EXTENDED_FRAME_BOUNDS_ 9
#define DWMWA_CLOAKED_               14

typedef HRESULT (WINAPI *DWMGETWINDOWATTRIBUTE)(HWND, DWORD, PVOID, DWORD);

#define MAX_WINDOWS 512

typedef struct {
    HWND hwnd;
    RECT rc;                     /* image coordinates */
} WINREC;

static WINREC g_wins[MAX_WINDOWS];
static int    g_nwins = 0;
static DWMGETWINDOWATTRIBUTE g_dwmGetAttr = NULL;
static int    g_dwmLoaded = 0;

static void LoadDwm(void)
{
    HMODULE h;
    if (g_dwmLoaded)
        return;
    g_dwmLoaded = 1;
    h = LoadLibrary("dwmapi.dll");          /* Vista and later only */
    if (h != NULL)
        g_dwmGetAttr = (DWMGETWINDOWATTRIBUTE)GetProcAddress(h, "DwmGetWindowAttribute");
}

/* Visible bounds of a top-level window: without the invisible resize borders
 * Windows 10/11 add, and FALSE for "cloaked" (hidden UWP) windows. */
static BOOL GetVisibleWindowRect(HWND hwnd, RECT *rc)
{
    if (g_dwmGetAttr != NULL) {
        DWORD cloaked = 0;
        if (g_dwmGetAttr(hwnd, DWMWA_CLOAKED_, &cloaked, sizeof(cloaked)) == S_OK && cloaked)
            return FALSE;
        if (g_dwmGetAttr(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS_, rc, sizeof(RECT)) == S_OK)
            return TRUE;
    }
    return GetWindowRect(hwnd, rc);
}

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp)
{
    const POINT *origin = (const POINT *)lp;
    DWORD pid = 0;
    LONG ex;
    RECT rc;

    if (g_nwins >= MAX_WINDOWS)
        return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return TRUE;
    ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
        return TRUE;                        /* click-through overlays (game bars and the like) */
    if (!GetVisibleWindowRect(hwnd, &rc))
        return TRUE;
    if (rc.right - rc.left < 2 || rc.bottom - rc.top < 2)
        return TRUE;
    OffsetRect(&rc, -origin->x, -origin->y);
    g_wins[g_nwins].hwnd = hwnd;
    g_wins[g_nwins].rc = rc;
    g_nwins++;
    return TRUE;
}

int CaptureScreen(LS_IMAGE *img, POINT *origin)
{
    int x, y, cx, cy, ok = 0;
    HDC screen, mem;
    HBITMAP bmp, old;
    BITMAPINFO bi;
    void *bits = NULL;
    OSVERSIONINFO ov;
    DWORD rop = SRCCOPY;

    img->bits = NULL;
    x  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y  = GetSystemMetrics(SM_YVIRTUALSCREEN);
    cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (cx <= 0 || cy <= 0) {               /* Windows 95 / NT 4: one monitor */
        x = y = 0;
        cx = GetSystemMetrics(SM_CXSCREEN);
        cy = GetSystemMetrics(SM_CYSCREEN);
    }
    origin->x = x;
    origin->y = y;

    /* CAPTUREBLT includes layered windows (2000/XP and later); older GDI rejects it. */
    ov.dwOSVersionInfoSize = sizeof(ov);
    if (GetVersionEx(&ov) && ov.dwPlatformId == VER_PLATFORM_WIN32_NT && ov.dwMajorVersion >= 5)
        rop |= CAPTUREBLT;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cx;
    bi.bmiHeader.biHeight = cy;             /* bottom-up, same layout as LS_IMAGE */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (mem != NULL && bmp != NULL && bits != NULL) {
        old = (HBITMAP)SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, cx, cy, screen, x, y, rop) ||
            BitBlt(mem, 0, 0, cx, cy, screen, x, y, SRCCOPY)) {
            GdiFlush();
            if (ImgAlloc(img, cx, cy)) {
                memcpy(img->bits, bits, (size_t)img->stride * (size_t)cy);
                ok = 1;
            }
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(NULL, screen);

    /* Snapshot the windows now, so the outlines match the frozen picture. */
    LoadDwm();
    g_nwins = 0;
    EnumWindows(EnumWinProc, (LPARAM)origin);
    return ok;
}

/* ====================================================================== overlay */

#define MODE_HOVER   0
#define MODE_PRESSED 1
#define MODE_DRAG    2
#define DRAG_SLOP    4

typedef struct {
    const LS_IMAGE *img;
    POINT origin;
    int w, h;
    HDC dcBright, dcDim, dcBack;
    HBITMAP bmBright, bmDim, bmBack, oldBright, oldDim, oldBack;
    HFONT font, oldFont;

    int mode;
    POINT anchor, cursor;
    RECT sel;                    /* drag rectangle or hovered window; empty = none */
    int selIsWindow;
    char title[100];             /* hovered window title, for the label */
    RECT lastDecor;
    RECT banner;                 /* primary monitor, image coordinates */

    int done, result;
    RECT out;
} OVERLAY;

static OVERLAY *g_ov = NULL;
static const char *OVERLAY_CLASS = "LegacyShotOverlay";
static const char *HELP_TEXT =
    "Drag to select an area  |  Click a window  |  Ctrl+click: a control  |  "
    "Space: whole screen  |  Esc: cancel";

static void NormRect(POINT a, POINT b, RECT *r)
{
    r->left   = a.x < b.x ? a.x : b.x;
    r->right  = (a.x > b.x ? a.x : b.x) + 1;
    r->top    = a.y < b.y ? a.y : b.y;
    r->bottom = (a.y > b.y ? a.y : b.y) + 1;
}

static void ClipToImage(OVERLAY *ov, RECT *r)
{
    RECT all;
    SetRect(&all, 0, 0, ov->w, ov->h);
    if (!IntersectRect(r, r, &all))
        SetRectEmpty(r);
}

/* Window (or, with Ctrl, control) under pt; fills ov->sel and ov->title. */
static void HitTest(OVERLAY *ov, POINT pt, int deep)
{
    int i;
    SetRectEmpty(&ov->sel);
    ov->title[0] = '\0';
    ov->selIsWindow = 1;
    for (i = 0; i < g_nwins; i++) {         /* EnumWindows order is top of Z order first */
        if (PtInRect(&g_wins[i].rc, pt)) {
            HWND h = g_wins[i].hwnd;
            ov->sel = g_wins[i].rc;
            if (deep && IsWindow(h)) {
                POINT spt;
                spt.x = pt.x + ov->origin.x;
                spt.y = pt.y + ov->origin.y;
                for (;;) {
                    POINT cpt = spt;
                    HWND child;
                    ScreenToClient(h, &cpt);
                    child = ChildWindowFromPointEx(h, cpt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
                    if (child == NULL || child == h)
                        break;
                    h = child;
                }
                if (h != g_wins[i].hwnd && GetWindowRect(h, &ov->sel))
                    OffsetRect(&ov->sel, -ov->origin.x, -ov->origin.y);
            }
            GetWindowText(h, ov->title, sizeof(ov->title));
            ClipToImage(ov, &ov->sel);
            return;
        }
    }
}

static void LabelRect(OVERLAY *ov, char *text, int cap, RECT *r)
{
    SIZE sz;
    int lw, lh;
    if (IsRectEmpty(&ov->sel)) {
        SetRectEmpty(r);
        text[0] = '\0';
        return;
    }
    if (ov->selIsWindow && ov->title[0])
        _snprintf(text, cap - 1, "%d x %d  -  %.60s", (int)(ov->sel.right - ov->sel.left),
                  (int)(ov->sel.bottom - ov->sel.top), ov->title);
    else
        _snprintf(text, cap - 1, "%d x %d", (int)(ov->sel.right - ov->sel.left),
                  (int)(ov->sel.bottom - ov->sel.top));
    text[cap - 1] = '\0';
    GetTextExtentPoint32(ov->dcBack, text, (int)strlen(text), &sz);
    lw = sz.cx + 10;
    lh = sz.cy + 4;
    r->left = ov->sel.left;
    r->top = ov->sel.top - lh - 3;
    if (r->top < 0)
        r->top = ov->sel.top + 3;           /* no room above: inside the top edge */
    if (r->left + lw > ov->w)
        r->left = ov->w - lw;
    if (r->left < 0)
        r->left = 0;
    r->right = r->left + lw;
    r->bottom = r->top + lh;
}

static void BannerRect(OVERLAY *ov, RECT *r)
{
    SIZE sz;
    int pw = GetSystemMetrics(SM_CXSCREEN), ph = GetSystemMetrics(SM_CYSCREEN);
    int px = -ov->origin.x, py = -ov->origin.y, bw, bh;
    RECT test;
    GetTextExtentPoint32(ov->dcBack, HELP_TEXT, (int)strlen(HELP_TEXT), &sz);
    bw = sz.cx + 24;
    bh = sz.cy + 10;
    r->left = px + (pw - bw) / 2;
    r->right = r->left + bw;
    r->top = py + 12;
    r->bottom = r->top + bh;
    test = *r;
    InflateRect(&test, 40, 40);
    if (PtInRect(&test, ov->cursor) || (ov->mode == MODE_DRAG && IntersectRect(&test, &test, &ov->sel))) {
        r->top = py + ph - bh - 12;         /* get out of the way */
        r->bottom = r->top + bh;
    }
}

static void DecorRect(OVERLAY *ov, RECT *d)
{
    char text[200];
    RECT lab, sel;
    SetRectEmpty(d);
    if (!IsRectEmpty(&ov->sel)) {
        sel = ov->sel;
        InflateRect(&sel, 2, 2);
        UnionRect(d, d, &sel);
        LabelRect(ov, text, sizeof(text), &lab);
        UnionRect(d, d, &lab);
    }
    if (ov->mode != MODE_DRAG) {
        BannerRect(ov, &ov->banner);
        UnionRect(d, d, &ov->banner);
    }
}

static void Refresh(HWND hwnd, OVERLAY *ov)
{
    RECT nd, u;
    DecorRect(ov, &nd);
    UnionRect(&u, &nd, &ov->lastDecor);
    if (!IsRectEmpty(&u))
        InvalidateRect(hwnd, &u, FALSE);
    ov->lastDecor = nd;
}

static void Paint(HWND hwnd, OVERLAY *ov)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r = ps.rcPaint, x;
    char text[200];

    BitBlt(ov->dcBack, r.left, r.top, r.right - r.left, r.bottom - r.top, ov->dcDim, r.left, r.top, SRCCOPY);

    if (!IsRectEmpty(&ov->sel)) {
        HPEN pen, oldPen;
        HBRUSH oldBrush;
        RECT lab;
        if (IntersectRect(&x, &ov->sel, &r))
            BitBlt(ov->dcBack, x.left, x.top, x.right - x.left, x.bottom - x.top,
                   ov->dcBright, x.left, x.top, SRCCOPY);

        /* Dotted black-and-white border just outside the selection. */
        pen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        oldPen = (HPEN)SelectObject(ov->dcBack, pen);
        oldBrush = (HBRUSH)SelectObject(ov->dcBack, GetStockObject(NULL_BRUSH));
        SetBkMode(ov->dcBack, OPAQUE);
        SetBkColor(ov->dcBack, RGB(0, 0, 0));
        Rectangle(ov->dcBack, ov->sel.left - 1, ov->sel.top - 1, ov->sel.right + 1, ov->sel.bottom + 1);
        SelectObject(ov->dcBack, oldBrush);
        SelectObject(ov->dcBack, oldPen);
        DeleteObject(pen);

        LabelRect(ov, text, sizeof(text), &lab);
        if (!IsRectEmpty(&lab)) {
            HBRUSH b = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(ov->dcBack, &lab, b);
            DeleteObject(b);
            SetBkMode(ov->dcBack, TRANSPARENT);
            SetTextColor(ov->dcBack, RGB(255, 255, 255));
            TextOut(ov->dcBack, lab.left + 5, lab.top + 2, text, (int)strlen(text));
        }
    }

    if (ov->mode != MODE_DRAG) {
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 225));
        HBRUSH oldBrush;
        BannerRect(ov, &ov->banner);
        oldBrush = (HBRUSH)SelectObject(ov->dcBack, b);
        SelectObject(ov->dcBack, GetStockObject(BLACK_PEN));
        Rectangle(ov->dcBack, ov->banner.left, ov->banner.top, ov->banner.right, ov->banner.bottom);
        SelectObject(ov->dcBack, oldBrush);
        DeleteObject(b);
        SetBkMode(ov->dcBack, TRANSPARENT);
        SetTextColor(ov->dcBack, RGB(0, 0, 0));
        TextOut(ov->dcBack, ov->banner.left + 12, ov->banner.top + 5, HELP_TEXT, (int)strlen(HELP_TEXT));
    }

    BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, ov->dcBack, r.left, r.top, SRCCOPY);
    EndPaint(hwnd, &ps);
}

static void Finish(OVERLAY *ov, const RECT *r, int ok)
{
    ov->done = 1;
    ov->result = ok;
    if (ok) {
        ov->out = *r;
        ClipToImage(ov, &ov->out);
        if (IsRectEmpty(&ov->out))
            ov->result = 0;
    }
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    OVERLAY *ov = g_ov;
    POINT pt;

    if (ov == NULL)
        return DefWindowProc(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd, ov);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_CROSS));
        return TRUE;

    case WM_MOUSEMOVE:
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ov->cursor = pt;
        if (ov->mode == MODE_PRESSED &&
            (labs(pt.x - ov->anchor.x) > DRAG_SLOP || labs(pt.y - ov->anchor.y) > DRAG_SLOP))
            ov->mode = MODE_DRAG;
        if (ov->mode == MODE_DRAG) {
            NormRect(ov->anchor, pt, &ov->sel);
            ClipToImage(ov, &ov->sel);
            ov->selIsWindow = 0;
        } else if (ov->mode == MODE_HOVER) {
            HitTest(ov, pt, (wp & MK_CONTROL) != 0);
        }
        Refresh(hwnd, ov);
        return 0;

    case WM_LBUTTONDOWN:
        ov->anchor.x = (short)LOWORD(lp);
        ov->anchor.y = (short)HIWORD(lp);
        ov->mode = MODE_PRESSED;
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd)
            ReleaseCapture();
        if (ov->mode == MODE_DRAG) {
            if (ov->sel.right - ov->sel.left >= 2 && ov->sel.bottom - ov->sel.top >= 2) {
                Finish(ov, &ov->sel, 1);
                return 0;
            }
        } else if (ov->mode == MODE_PRESSED && ov->selIsWindow && !IsRectEmpty(&ov->sel)) {
            Finish(ov, &ov->sel, 1);
            return 0;
        }
        ov->mode = MODE_HOVER;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        HitTest(ov, pt, (wp & MK_CONTROL) != 0);
        Refresh(hwnd, ov);
        return 0;

    case WM_RBUTTONUP:
        if (ov->mode == MODE_DRAG) {        /* right-click during a drag drops the drag */
            if (GetCapture() == hwnd) ReleaseCapture();
            ov->mode = MODE_HOVER;
            HitTest(ov, ov->cursor, 0);
            Refresh(hwnd, ov);
        } else {
            Finish(ov, NULL, 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
            Finish(ov, NULL, 0);
        } else if (msg == WM_KEYDOWN && (wp == VK_SPACE || wp == VK_RETURN) && ov->mode != MODE_DRAG) {
            RECT all;
            SetRect(&all, 0, 0, ov->w, ov->h);
            Finish(ov, &all, 1);
        } else if (wp == VK_CONTROL && ov->mode == MODE_HOVER) {
            HitTest(ov, ov->cursor, msg == WM_KEYDOWN);
            Refresh(hwnd, ov);
        }
        return 0;

    case WM_CLOSE:
        Finish(ov, NULL, 0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* 24-bit DIB section holding a copy of img (dimmed if dim != 0), selected into a new DC. */
static int MakeDibDC(HDC ref, const LS_IMAGE *img, int dim, HDC *dc, HBITMAP *bmp, HBITMAP *old)
{
    BITMAPINFO bi;
    void *bits = NULL;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img->width;
    bi.bmiHeader.biHeight = img->height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    *dc = CreateCompatibleDC(ref);
    *bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (*dc == NULL || *bmp == NULL || bits == NULL)
        return 0;
    if (dim) {
        const unsigned char *s = img->bits;
        unsigned char *d = (unsigned char *)bits;
        unsigned long n = (unsigned long)img->stride * (unsigned long)img->height;
        while (n--) {
            *d++ = (unsigned char)((*s >> 1) + (*s >> 4));   /* about 56 % brightness */
            s++;
        }
    } else {
        memcpy(bits, img->bits, (size_t)img->stride * (size_t)img->height);
    }
    *old = (HBITMAP)SelectObject(*dc, *bmp);
    return 1;
}

int SelectRegion(HINSTANCE inst, const LS_IMAGE *img, POINT origin, RECT *sel)
{
    static int registered = 0;
    OVERLAY ov;
    HWND hwnd;
    HDC screen;
    MSG msg;
    int ok;

    if (!registered) {
        WNDCLASS wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        wc.lpszClassName = OVERLAY_CLASS;
        if (!RegisterClass(&wc))
            return 0;
        registered = 1;
    }

    memset(&ov, 0, sizeof(ov));
    ov.img = img;
    ov.origin = origin;
    ov.w = img->width;
    ov.h = img->height;

    screen = GetDC(NULL);
    ok = MakeDibDC(screen, img, 0, &ov.dcBright, &ov.bmBright, &ov.oldBright) &&
         MakeDibDC(screen, img, 1, &ov.dcDim, &ov.bmDim, &ov.oldDim);
    if (ok) {
        ov.dcBack = CreateCompatibleDC(screen);
        ov.bmBack = CreateCompatibleBitmap(screen, ov.w, ov.h);
        ok = ov.dcBack != NULL && ov.bmBack != NULL;
        if (ok)
            ov.oldBack = (HBITMAP)SelectObject(ov.dcBack, ov.bmBack);
    }
    ReleaseDC(NULL, screen);

    ov.result = 0;
    if (ok) {
        ov.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        ov.oldFont = (HFONT)SelectObject(ov.dcBack, ov.font);
        BitBlt(ov.dcBack, 0, 0, ov.w, ov.h, ov.dcDim, 0, 0, SRCCOPY);

        g_ov = &ov;
        hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OVERLAY_CLASS, "LegacyShot",
                              WS_POPUP, origin.x, origin.y, ov.w, ov.h, NULL, NULL, inst, NULL);
        if (hwnd != NULL) {
            POINT cur;
            GetCursorPos(&cur);
            ov.cursor.x = cur.x - origin.x;
            ov.cursor.y = cur.y - origin.y;
            HitTest(&ov, ov.cursor, 0);
            DecorRect(&ov, &ov.lastDecor);
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            UpdateWindow(hwnd);

            while (!ov.done) {
                if (!GetMessage(&msg, NULL, 0, 0)) {
                    PostQuitMessage((int)msg.wParam);   /* let the main loop see WM_QUIT too */
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (GetCapture() == hwnd)
                ReleaseCapture();
            DestroyWindow(hwnd);
        }
        g_ov = NULL;
        SelectObject(ov.dcBack, ov.oldFont);
    }

    if (ov.dcBack)   { if (ov.oldBack) SelectObject(ov.dcBack, ov.oldBack); DeleteDC(ov.dcBack); }
    if (ov.bmBack)   DeleteObject(ov.bmBack);
    if (ov.dcBright) { if (ov.oldBright) SelectObject(ov.dcBright, ov.oldBright); DeleteDC(ov.dcBright); }
    if (ov.bmBright) DeleteObject(ov.bmBright);
    if (ov.dcDim)    { if (ov.oldDim) SelectObject(ov.dcDim, ov.oldDim); DeleteDC(ov.dcDim); }
    if (ov.bmDim)    DeleteObject(ov.bmDim);

    if (ov.result)
        *sel = ov.out;
    return ov.result;
}

/* ====================================================================== clipboard */

int CopyImageToClipboard(HWND owner, const LS_IMAGE *img)
{
    unsigned long size = (unsigned long)img->stride * (unsigned long)img->height;
    HGLOBAL hg;
    BITMAPINFOHEADER *bih;

    if (!OpenClipboard(owner))
        return 0;
    EmptyClipboard();
    hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, sizeof(BITMAPINFOHEADER) + size);
    if (hg == NULL) {
        CloseClipboard();
        return 0;
    }
    bih = (BITMAPINFOHEADER *)GlobalLock(hg);
    memset(bih, 0, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = img->width;
    bih->biHeight = img->height;
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = size;
    memcpy(bih + 1, img->bits, size);
    GlobalUnlock(hg);
    if (SetClipboardData(CF_DIB, hg) == NULL) {
        GlobalFree(hg);
        CloseClipboard();
        return 0;
    }
    CloseClipboard();
    return 1;
}
