/*
 * main.c - LegacyShot for Visual C++: a Lightshot-style screenshot tool that
 * uploads to a LegacyShot (LSUP/2) server.
 *
 *   Print Screen (or a click on the tray icon) freezes the screen; drag an
 *   area or click a window. A dialog then offers Upload, Save As, Copy and
 *   Reselect, with PNG, GIF or BMP encoded on the client. Uploading shows a
 *   progress bar, bytes sent, rate and time left, then the link.
 *
 * Build: VC++ 6.0 (LegacyShot.dsp or build.bat). Runs on Windows 95/98/ME,
 *        NT 4.0 and later. Links wsock32, comctl32, comdlg32, shell32 and
 *        the static multithreaded CRT, so it has no DLL dependencies.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "resource.h"
#include "lsup.h"
#include "imgenc.h"
#include "capture.h"

#define APP_NAME        "LegacyShot"
#define MAIN_CLASS      "LegacyShotVCMain"
#define MUTEX_NAME      "LegacyShotVC.SingleInstance"

#define WM_TRAY         (WM_APP + 1)
#define WM_LS_STATUS    (WM_APP + 2)    /* lParam = malloc'd text for the status line */
#define WM_LS_INFO      (WM_APP + 3)    /* lParam = malloc'd text for the file line */
#define WM_LS_DONE      (WM_APP + 4)    /* wParam = result code, lParam = malloc'd text */

#define HOTKEY_PRTSC    1
#define HOTKEY_OTHER    2
#define TIMER_CAP_AREA  1
#define TIMER_CAP_FULL  2
#define TIMER_PROGRESS  3

#define SHOT_CLOSE      1
#define SHOT_RESELECT   2
#define SHOT_UPLOADED   3

#define MAX_ATTEMPTS    4
#define RATE_SAMPLES    9               /* 9 x 0.5 s: rate averaged over about 4 seconds */
#define MAX_FILE_UPLOAD (64UL * 1024 * 1024)

#ifndef OPENFILENAME_SIZE_VERSION_400
#define OPENFILENAME_SIZE_VERSION_400 sizeof(OPENFILENAME)
#endif
#ifndef NOTIFYICONDATA_V1_SIZE
#define NOTIFYICONDATA_V1_SIZE sizeof(NOTIFYICONDATA)
#endif

/* ====================================================================== settings */

typedef struct {
    char host[256];
    int  port;
    char user[256];
    char key[256];
    int  link;                          /* index into g_links */
    int  format;                        /* IMG_FMT_* */
    int  copyLink;
    int  openLink;
    int  usePrtSc;
    WORD hotkey;                        /* hotkey control format: LOBYTE vk, HIBYTE HOTKEYF_* */
} SETTINGS;

static const struct {
    const char *name;
    unsigned long chunk;
    int window;
    int timeout;
} g_links[3] = {
    { "Dial-up modem (28.8 - 56k)",      4096,  2, 60 },
    { "ISDN or slow broadband",          16384, 3, 30 },
    { "Broadband / LAN",                 32768, 4, 20 }
};

static const char *g_formatNames[3] = { "PNG", "GIF", "BMP" };
static const char *g_formatExt[3]   = { ".png", ".gif", ".bmp" };
static const int   g_formatType[3]  = { LSUP_IMG_PNG, LSUP_IMG_GIF, LSUP_IMG_BMP };

static HINSTANCE g_inst;
static HWND      g_main;
static SETTINGS  g_set;
static char      g_ini[MAX_PATH];
static int       g_busy = 0;
static UINT      g_taskbarCreated = 0;
static NOTIFYICONDATA g_nid;
static int       g_isNT = 0;

static void LoadSettings(void)
{
    char *dot;
    GetModuleFileName(NULL, g_ini, sizeof(g_ini) - 5);
    dot = strrchr(g_ini, '.');
    if (dot != NULL && strchr(dot, '\\') == NULL)
        *dot = '\0';
    strcat(g_ini, ".ini");

    GetPrivateProfileString("Server", "Host", "", g_set.host, sizeof(g_set.host), g_ini);
    g_set.port = GetPrivateProfileInt("Server", "Port", LSUP_PORT, g_ini);
    GetPrivateProfileString("Server", "User", "", g_set.user, sizeof(g_set.user), g_ini);
    GetPrivateProfileString("Server", "ApiKey", "", g_set.key, sizeof(g_set.key), g_ini);
    g_set.link     = GetPrivateProfileInt("Options", "Connection", 2, g_ini);
    g_set.format   = GetPrivateProfileInt("Options", "Format", IMG_FMT_PNG, g_ini);
    g_set.copyLink = GetPrivateProfileInt("Options", "CopyLink", 1, g_ini);
    g_set.openLink = GetPrivateProfileInt("Options", "OpenLink", 0, g_ini);
    g_set.usePrtSc = GetPrivateProfileInt("Options", "PrintScreen", 1, g_ini);
    g_set.hotkey   = (WORD)GetPrivateProfileInt("Options", "Hotkey", 0, g_ini);
    if (g_set.link < 0 || g_set.link > 2) g_set.link = 2;
    if (g_set.format < 0 || g_set.format > 2) g_set.format = IMG_FMT_PNG;
    if (g_set.port <= 0 || g_set.port > 65535) g_set.port = LSUP_PORT;
}

static void WriteInt(const char *sec, const char *key, int v)
{
    char buf[16];
    sprintf(buf, "%d", v);
    WritePrivateProfileString(sec, key, buf, g_ini);
}

static void SaveSettings(void)
{
    WritePrivateProfileString("Server", "Host", g_set.host, g_ini);
    WriteInt("Server", "Port", g_set.port);
    WritePrivateProfileString("Server", "User", g_set.user, g_ini);
	WritePrivateProfileString("Server", "ApiKey", g_set.key, g_ini);
    WriteInt("Options", "Connection", g_set.link);
    WriteInt("Options", "Format", g_set.format);
    WriteInt("Options", "CopyLink", g_set.copyLink);
    WriteInt("Options", "OpenLink", g_set.openLink);
    WriteInt("Options", "PrintScreen", g_set.usePrtSc);
    WriteInt("Options", "Hotkey", g_set.hotkey);
}

static int SettingsComplete(void)
{
    return g_set.host[0] && g_set.user[0] && g_set.key[0];
}

/* ====================================================================== helpers */

static int IniExists(void)
{
	DWORD a = GetFileAttributes(g_ini);
	return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static char *DupText(const char *fmt, ...)
{
    char buf[600], *p;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    p = (char *)malloc(strlen(buf) + 1);
    if (p) strcpy(p, buf);
    return p;
}

static void PostText(HWND hwnd, UINT msg, WPARAM wp, char *text)
{
    if (!PostMessage(hwnd, msg, wp, (LPARAM)text) && text)
        free(text);
}

static void FormatBytes(unsigned long n, char *out)
{
    if (n < 1024UL)                 sprintf(out, "%lu bytes", n);
    else if (n < 1024UL * 1024UL)   sprintf(out, "%.1f KB", n / 1024.0);
    else                            sprintf(out, "%.2f MB", n / (1024.0 * 1024.0));
}

static void FormatRate(double bps, char *out)
{
    if (bps < 1024.0)               sprintf(out, "%.0f bytes/s", bps);
    else if (bps < 1024.0 * 1024.0) sprintf(out, "%.1f KB/s", bps / 1024.0);
    else                            sprintf(out, "%.2f MB/s", bps / (1024.0 * 1024.0));
}

static void FormatTime(unsigned long sec, char *out)
{
    if (sec >= 3600) sprintf(out, "%lu:%02lu:%02lu", sec / 3600, (sec / 60) % 60, sec % 60);
    else             sprintf(out, "%lu:%02lu", sec / 60, sec % 60);
}

static int SetClipboardText(HWND owner, const char *text)
{
    HGLOBAL hg;
    size_t n = strlen(text) + 1;
    if (!OpenClipboard(owner))
        return 0;
    EmptyClipboard();
    hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, n);
    if (hg != NULL) {
        memcpy(GlobalLock(hg), text, n);
        GlobalUnlock(hg);
        if (SetClipboardData(CF_TEXT, hg) == NULL) {
            GlobalFree(hg);
            hg = NULL;
        }
    }
    CloseClipboard();
    return hg != NULL;
}

/* Text for the wire is Windows-1252 (spec). Convert from the ANSI code page if it differs. */
static void ToCp1252(const char *in, char *out, int cap)
{
    WCHAR wide[1100];
    int n;
    if (GetACP() == 1252) {
        strncpy(out, in, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    n = MultiByteToWideChar(CP_ACP, 0, in, -1, wide, sizeof(wide) / sizeof(wide[0]));
    if (n <= 0 || WideCharToMultiByte(1252, 0, wide, -1, out, cap, "?", NULL) <= 0) {
        strncpy(out, in, cap - 1);
        out[cap - 1] = '\0';
    }
}

/* The server accepts [A-Za-z0-9._ -], up to 100 characters. */
static void SanitizeName(const char *in, const char *ext, char *out)
{
    int n = 0;
    for (; *in && n < 90; in++) {
        char c = *in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == ' ' || c == '-')
            out[n++] = c;
        else
            out[n++] = '_';
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    if (n == 0) {
        strcpy(out, "shot");
        n = 4;
    }
    out[n] = '\0';
    if (ext != NULL) {
        size_t el = strlen(ext);
        if ((size_t)n < el || _stricmp(out + n - el, ext) != 0)
            strcat(out, ext);
    }
}

static void DefaultName(char *out)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    sprintf(out, "shot_%04u%02u%02u_%02u%02u%02u",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static int WriteWholeFile(const char *path, const unsigned char *data, unsigned long size)
{
    DWORD wrote = 0;
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, data, size, &wrote, NULL) && wrote == size;
    CloseHandle(h);
    if (!ok)
        DeleteFile(path);
    return ok;
}

static const char *ImgErrorText(int rc)
{
    switch (rc) {
    case IMG_E_NOMEM:  return "Not enough memory to encode the image.";
    case IMG_E_SIZE:   return "The image is too large for this format.";
    case IMG_E_CANCEL: return "Cancelled.";
    default:           return "The image could not be encoded.";
    }
}

/* ====================================================================== upload worker */

#define PH_IDLE    0
#define PH_ENCODE  1
#define PH_CONNECT 2
#define PH_UPLOAD  3

typedef struct {
    /* Input. Either img (encoded here in `format`) or fileData (sent as is). */
    const LS_IMAGE *img;
    int format;
    const unsigned char *fileData;
    unsigned long fileSize;
    int imageType;
    char name[128];                     /* sanitized, with extension */
    char desc[1024];                    /* Windows-1252 */
    SETTINGS s;                         /* snapshot, so Settings can change meanwhile */

    HWND dlg;
    HANDLE thread;
    int running;

    /* Shared with the worker: written there, read by the dialog's timer. */
    volatile long phase, done, total, rateReset;
    volatile long cancel;

    /* Rate samples (dialog thread only) */
    DWORD sampleTick[RATE_SAMPLES];
    unsigned long sampleBytes[RATE_SAMPLES];
    int nsamples;

    LS_BUF enc;
    char reference[512];
    int result;
} UPLOAD_JOB;

static void EncodeProgress(void *ctx, unsigned long done, unsigned long total)
{
    UPLOAD_JOB *j = (UPLOAD_JOB *)ctx;
    j->total = (long)total;
    j->done = (long)done;
}

static void SendProgress(void *ctx, int phase, unsigned long done, unsigned long total)
{
    UPLOAD_JOB *j = (UPLOAD_JOB *)ctx;
    char kb[32];
    j->total = (long)total;
    j->done = (long)done;
    if (phase == LSUP_PH_START) {
        j->rateReset = 1;
        if (done > 0) {
            FormatBytes(done, kb);
            PostText(j->dlg, WM_LS_STATUS, 0, DupText("Resuming: the server already has %s.", kb));
        } else {
            PostText(j->dlg, WM_LS_STATUS, 0, DupText("Uploading to %s...", j->s.host));
        }
    }
}

static unsigned __stdcall UploadThread(void *arg)
{
    UPLOAD_JOB *j = (UPLOAD_JOB *)arg;
    const unsigned char *data;
    unsigned long size;
    unsigned char md5[16];
    LSUP_CONN *c = NULL;
    char size1[32], err[300];
    int rc = LSUP_OK, attempt, type;

    err[0] = '\0';
    if (j->img != NULL) {
        j->phase = PH_ENCODE;
        PostText(j->dlg, WM_LS_STATUS, 0, DupText("Encoding %s...", g_formatNames[j->format]));
        rc = ImgEncode(j->format, j->img, &j->enc, EncodeProgress, j, &j->cancel);
        if (rc != IMG_OK) {
            strcpy(err, ImgErrorText(rc));
            rc = (rc == IMG_E_CANCEL) ? LSUP_E_CANCELLED : LSUP_E_ARG;
            goto finish;
        }
        data = j->enc.data;
        size = j->enc.size;
        type = g_formatType[j->format];
        j->total = 0;                   /* empty bar until the upload starts */
        j->done = 0;
    } else {
        data = j->fileData;
        size = j->fileSize;
        type = j->imageType;
    }
    FormatBytes(size, size1);
    PostText(j->dlg, WM_LS_INFO, 0, DupText("%s  -  %s", j->name, size1));
    LsupMd5(data, size, md5);

    c = LsupNew();
    if (c == NULL) {
        strcpy(err, "Not enough memory.");
        rc = LSUP_E_ARG;
        goto finish;
    }
    c->cancel = &j->cancel;
    c->timeoutSec = g_links[j->s.link].timeout;

    for (attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        int wait, i;
        j->phase = PH_CONNECT;
        PostText(j->dlg, WM_LS_STATUS, 0, DupText("Connecting to %s:%d...", j->s.host, j->s.port));
        rc = LsupConnect(c, j->s.host, (unsigned short)j->s.port);
        if (rc == LSUP_OK) {
            PostText(j->dlg, WM_LS_STATUS, 0, DupText("Logging in as %s...", j->s.user));
            rc = LsupLogin(c, j->s.user, j->s.key);
        }
        if (rc == LSUP_OK) {
            j->phase = PH_UPLOAD;
            rc = LsupUpload(c, data, size, md5, type, j->name, j->desc,
                            g_links[j->s.link].chunk, g_links[j->s.link].window,
                            SendProgress, j, j->reference, sizeof(j->reference));
        }
        if (rc == LSUP_OK) {
            LsupLogout(c);
            break;
        }
        strcpy(err, c->err);
        LsupClose(c);
        if (rc == LSUP_E_CANCELLED || !LsupIsRetryable(rc) || attempt == MAX_ATTEMPTS)
            break;

        /* Dropped line, busy server, damaged upload: wait, reconnect, resume. */
        wait = (rc == LSUP_ERR_SERVERBUSY) ? 45 : (rc == LSUP_ERR_MD5MISMATCH) ? 1 : 5;
        PostText(j->dlg, WM_LS_STATUS, 0, DupText("%s\nRetrying in %d s (attempt %d of %d)...",
                                                  err, wait, attempt + 1, MAX_ATTEMPTS));
        for (i = 0; i < wait * 4 && !j->cancel; i++)
            Sleep(250);
        if (j->cancel) {
            rc = LSUP_E_CANCELLED;
            break;
        }
    }

finish:
    if (rc == LSUP_E_CANCELLED)
        PostText(j->dlg, WM_LS_DONE, (WPARAM)rc,
                 DupText("Cancelled. Uploading the same picture again within 48 hours resumes where it stopped."));
    else if (rc == LSUP_OK)
        PostText(j->dlg, WM_LS_DONE, (WPARAM)rc, DupText("%s", j->reference));
    else
        PostText(j->dlg, WM_LS_DONE, (WPARAM)rc, DupText("%s", err[0] ? err : "The upload failed."));
    if (c != NULL)
        LsupFree(c);
    return 0;
}

/* ====================================================================== upload dialog */

static void UpdateProgressUI(HWND dlg, UPLOAD_JOB *j)
{
    long phase = j->phase, done = j->done, total = j->total;
    char a[40], b[40], line[100];
    int pos = 0;

    if (total > 0)
        pos = (int)((double)done * 1000.0 / (double)total);
    SendDlgItemMessage(dlg, IDC_PROGRESS, PBM_SETPOS, (WPARAM)pos, 0);

    if (phase != PH_UPLOAD) {
        if (phase == PH_ENCODE && total > 0) {
            sprintf(line, "encoding, %d%%", pos / 10);
            SetDlgItemText(dlg, IDC_BYTES, line);
        }
        return;
    }

    FormatBytes((unsigned long)done, a);
    FormatBytes((unsigned long)total, b);
    sprintf(line, "%s of %s", a, b);
    SetDlgItemText(dlg, IDC_BYTES, line);

    /* Rate over the last few seconds; restart the window when an upload (re)starts. */
    if (j->rateReset) {
        j->rateReset = 0;
        j->nsamples = 0;
    }
    if (j->nsamples == RATE_SAMPLES) {
        memmove(j->sampleTick, j->sampleTick + 1, sizeof(DWORD) * (RATE_SAMPLES - 1));
        memmove(j->sampleBytes, j->sampleBytes + 1, sizeof(unsigned long) * (RATE_SAMPLES - 1));
        j->nsamples--;
    }
    j->sampleTick[j->nsamples] = GetTickCount();
    j->sampleBytes[j->nsamples] = (unsigned long)done;
    j->nsamples++;

    if (j->nsamples >= 3) {
        DWORD dt = j->sampleTick[j->nsamples - 1] - j->sampleTick[0];
        unsigned long db = j->sampleBytes[j->nsamples - 1] - j->sampleBytes[0];
        if (dt >= 900) {
            double rate = (double)db * 1000.0 / (double)dt;
            FormatRate(rate, a);
            SetDlgItemText(dlg, IDC_RATE, a);
            if (rate > 1.0) {
                FormatTime((unsigned long)((double)(total - done) / rate + 0.5), a);
                SetDlgItemText(dlg, IDC_ETA, a);
            } else {
                SetDlgItemText(dlg, IDC_ETA, "waiting for the server...");
            }
        }
    }
}

static BOOL CALLBACK UploadDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    UPLOAD_JOB *j = (UPLOAD_JOB *)GetWindowLong(dlg, DWL_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        unsigned tid;
        j = (UPLOAD_JOB *)lp;
        SetWindowLong(dlg, DWL_USER, (LONG)j);
        j->dlg = dlg;
        SetDlgItemText(dlg, IDC_FILEINFO, j->name);
        SendDlgItemMessage(dlg, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        j->running = 1;
        j->thread = (HANDLE)_beginthreadex(NULL, 0, UploadThread, j, 0, &tid);
        if (j->thread == NULL) {
            j->running = 0;
            SetDlgItemText(dlg, IDC_STATUS, "Could not start the upload thread.");
            SetDlgItemText(dlg, IDCANCEL, "Close");
        } else {
            SetTimer(dlg, TIMER_PROGRESS, 500, NULL);
        }
        SetForegroundWindow(dlg);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == TIMER_PROGRESS)
            UpdateProgressUI(dlg, j);
        return TRUE;

    case WM_LS_STATUS:
    case WM_LS_INFO:
        SetDlgItemText(dlg, msg == WM_LS_STATUS ? IDC_STATUS : IDC_FILEINFO, (const char *)lp);
        free((void *)lp);
        return TRUE;

    case WM_LS_DONE: {
        char *text = (char *)lp;
        int rc = (int)wp;
        KillTimer(dlg, TIMER_PROGRESS);
        if (j->thread != NULL) {
            WaitForSingleObject(j->thread, INFINITE);   /* it posted this as its last act */
            CloseHandle(j->thread);
            j->thread = NULL;
        }
        j->running = 0;
        j->result = rc;
        UpdateProgressUI(dlg, j);
        if (rc == LSUP_OK) {
            const char *how = "";
            SendDlgItemMessage(dlg, IDC_PROGRESS, PBM_SETPOS, 1000, 0);
            SetDlgItemText(dlg, IDC_ETA, "done");
            SetDlgItemText(dlg, IDC_LINK, text);
            EnableWindow(GetDlgItem(dlg, IDC_COPYLINK), TRUE);
            EnableWindow(GetDlgItem(dlg, IDC_OPENLINK), TRUE);
            if (j->s.copyLink && SetClipboardText(dlg, text))
                how = "Uploaded. The link is on the clipboard.";
            else
                how = "Uploaded.";
            SetDlgItemText(dlg, IDC_STATUS, how);
            if (j->s.openLink && _strnicmp(text, "http", 4) == 0)
                ShellExecute(dlg, "open", text, NULL, NULL, SW_SHOWNORMAL);
            SendDlgItemMessage(dlg, IDC_LINK, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, IDC_LINK));
        } else {
            SetDlgItemText(dlg, IDC_STATUS, text ? text : "The upload failed.");
            SetDlgItemText(dlg, IDC_ETA, "-");
        }
        SetDlgItemText(dlg, IDCANCEL, "Close");
        EnableWindow(GetDlgItem(dlg, IDCANCEL), TRUE);
        if (text) free(text);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDCANCEL:
            if (j->running) {
                j->cancel = 1;
                SetDlgItemText(dlg, IDC_STATUS, "Cancelling...");
                EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
            } else {
                EndDialog(dlg, j->result == LSUP_OK);
            }
            return TRUE;
        case IDC_COPYLINK: {
            char link[512];
            GetDlgItemText(dlg, IDC_LINK, link, sizeof(link));
            if (SetClipboardText(dlg, link))
                SetDlgItemText(dlg, IDC_STATUS, "Uploaded. The link is on the clipboard.");
            return TRUE;
        }
        case IDC_OPENLINK: {
            char link[512];
            GetDlgItemText(dlg, IDC_LINK, link, sizeof(link));
            if ((int)(size_t)ShellExecute(dlg, "open", link, NULL, NULL, SW_SHOWNORMAL) <= 32)
                SetDlgItemText(dlg, IDC_STATUS, "Windows could not open the link; copy it instead.");
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        SendMessage(dlg, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    return FALSE;
}

/* Runs the upload dialog; returns 1 if the upload succeeded. */
static int RunUpload(HWND owner, UPLOAD_JOB *j)
{
    int ok;
    j->s = g_set;
    ok = (int)DialogBoxParam(g_inst, MAKEINTRESOURCE(IDD_UPLOAD), owner, (DLGPROC)UploadDlgProc, (LPARAM)j);
    BufFree(&j->enc);
    return ok == 1;
}

/* ====================================================================== settings dialog */

static void ReadSettingsDlg(HWND dlg, SETTINGS *s)
{
    *s = g_set;
    GetDlgItemText(dlg, IDC_HOST, s->host, sizeof(s->host));
    s->port = (int)GetDlgItemInt(dlg, IDC_PORT, NULL, FALSE);
    GetDlgItemText(dlg, IDC_USER, s->user, sizeof(s->user));
    GetDlgItemText(dlg, IDC_KEY, s->key, sizeof(s->key));
    s->link = (int)SendDlgItemMessage(dlg, IDC_LINKPROFILE, CB_GETCURSEL, 0, 0);
    s->format = (int)SendDlgItemMessage(dlg, IDC_DEFFORMAT, CB_GETCURSEL, 0, 0);
    s->copyLink = IsDlgButtonChecked(dlg, IDC_COPYLINKAUTO) == BST_CHECKED;
    s->openLink = IsDlgButtonChecked(dlg, IDC_OPENLINKAUTO) == BST_CHECKED;
    s->usePrtSc = IsDlgButtonChecked(dlg, IDC_USEPRTSC) == BST_CHECKED;
    s->hotkey = (WORD)SendDlgItemMessage(dlg, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0);
    if (s->link < 0) s->link = 2;
    if (s->format < 0) s->format = IMG_FMT_PNG;
}

static int RegisterHotkeys(void);

static BOOL CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    int i;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg, IDC_HOST, g_set.host);
        SetDlgItemInt(dlg, IDC_PORT, (UINT)g_set.port, FALSE);
        SetDlgItemText(dlg, IDC_USER, g_set.user);
        SetDlgItemText(dlg, IDC_KEY, g_set.key);
        SendDlgItemMessage(dlg, IDC_HOST, EM_LIMITTEXT, 255, 0);
        SendDlgItemMessage(dlg, IDC_USER, EM_LIMITTEXT, 255, 0);
        SendDlgItemMessage(dlg, IDC_KEY, EM_LIMITTEXT, 255, 0);
        SendDlgItemMessage(dlg, IDC_PORT, EM_LIMITTEXT, 5, 0);
        for (i = 0; i < 3; i++)
            SendDlgItemMessage(dlg, IDC_LINKPROFILE, CB_ADDSTRING, 0, (LPARAM)g_links[i].name);
        SendDlgItemMessage(dlg, IDC_LINKPROFILE, CB_SETCURSEL, (WPARAM)g_set.link, 0);
        SendDlgItemMessage(dlg, IDC_DEFFORMAT, CB_ADDSTRING, 0, (LPARAM)"PNG (recommended)");
        SendDlgItemMessage(dlg, IDC_DEFFORMAT, CB_ADDSTRING, 0, (LPARAM)"Compuserve GIF");
        SendDlgItemMessage(dlg, IDC_DEFFORMAT, CB_ADDSTRING, 0, (LPARAM)"BMP");
        SendDlgItemMessage(dlg, IDC_DEFFORMAT, CB_SETCURSEL, (WPARAM)g_set.format, 0);
        CheckDlgButton(dlg, IDC_COPYLINKAUTO, g_set.copyLink ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_OPENLINKAUTO, g_set.openLink ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_USEPRTSC, g_set.usePrtSc ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessage(dlg, IDC_HOTKEY, HKM_SETHOTKEY, g_set.hotkey, 0);
        SetForegroundWindow(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TESTLOGIN: {
            SETTINGS s;
            LSUP_CONN *c;
            HCURSOR old;
            int rc;
            ReadSettingsDlg(dlg, &s);
            if (!s.host[0] || !s.user[0] || !s.key[0]) {
                SetDlgItemText(dlg, IDC_TESTRESULT, "Enter the host, user name and API key first.");
                return TRUE;
            }
            c = LsupNew();
            if (c == NULL) return TRUE;
            SetDlgItemText(dlg, IDC_TESTRESULT, "Connecting...");
            UpdateWindow(dlg);
            old = SetCursor(LoadCursor(NULL, IDC_WAIT));
            c->timeoutSec = 15;
            rc = LsupConnect(c, s.host, (unsigned short)s.port);
            if (rc == LSUP_OK)
                rc = LsupLogin(c, s.user, s.key);
            if (rc == LSUP_OK) {
                char t[160], mx[32];
                FormatBytes(c->maxFileSize, mx);
                sprintf(t, "Login OK. Server accepts images up to %s.", mx);
                SetDlgItemText(dlg, IDC_TESTRESULT, t);
                LsupLogout(c);
            } else {
                SetDlgItemText(dlg, IDC_TESTRESULT, c->err);
            }
            LsupFree(c);
            SetCursor(old);
            return TRUE;
        }
        case IDOK: {
            SETTINGS s;
            ReadSettingsDlg(dlg, &s);
            if (s.port <= 0 || s.port > 65535) {
                MessageBox(dlg, "The port must be between 1 and 65535 (Default port is 7745).",
                           APP_NAME, MB_ICONEXCLAMATION);
                return TRUE;
            }
            g_set = s;
            SaveSettings();
            if (!RegisterHotkeys())
                MessageBox(dlg, "One of the hotkeys is already used by another program "
                           "(screenshot tools often take Print Screen). Choose another hotkey, "
                           "or click the LegacyShot tray icon to capture.", APP_NAME, MB_ICONINFORMATION);
            EndDialog(dlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static int RunSettings(HWND owner)
{
    return (int)DialogBoxParam(g_inst, MAKEINTRESOURCE(IDD_SETTINGS), owner, (DLGPROC)SettingsDlgProc, 0) == 1;
}

/* ====================================================================== shot dialog */

typedef struct {
    LS_IMAGE *img;
    int colors;                         /* 1..256, or 257 for more */
} SHOT_CTX;

static int SelectedFormat(HWND dlg)
{
    if (IsDlgButtonChecked(dlg, IDC_FMT_GIF) == BST_CHECKED) return IMG_FMT_GIF;
    if (IsDlgButtonChecked(dlg, IDC_FMT_BMP) == BST_CHECKED) return IMG_FMT_BMP;
    return IMG_FMT_PNG;
}

static void UpdateHint(HWND dlg, SHOT_CTX *sc)
{
    char t[200], sz[32];
    switch (SelectedFormat(dlg)) {
    case IMG_FMT_GIF:
        if (sc->colors <= 256)
            sprintf(t, "", sc->colors);
        else
            strcpy(t, "Warning: This picture has more than 256 colors; Choosing GIF will reduce them.");
        break;
    case IMG_FMT_BMP:
        FormatBytes(54 + (unsigned long)sc->img->stride * (unsigned long)sc->img->height, sz);
        sprintf(t, "Warning: Might take a while on slower connections.;", sz);
        break;
    default:
        strcpy(t, "");
        break;
    }
    SetDlgItemText(dlg, IDC_FMT_HINT, t);
}

static void DrawPreview(const DRAWITEMSTRUCT *di, const LS_IMAGE *img)
{
    RECT r = di->rcItem, f;
    int bw = r.right - r.left - 4, bh = r.bottom - r.top - 4, dw, dh, x, y;
    BITMAPINFO bi;

    FillRect(di->hDC, &r, GetSysColorBrush(COLOR_APPWORKSPACE));
    if ((long)img->width * bh > (long)img->height * bw) {   /* width-limited */
        dw = img->width < bw ? img->width : bw;
        dh = (int)((long)img->height * dw / img->width);
    } else {
        dh = img->height < bh ? img->height : bh;
        dw = (int)((long)img->width * dh / img->height);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    x = r.left + (r.right - r.left - dw) / 2;
    y = r.top + (r.bottom - r.top - dh) / 2;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img->width;
    bi.bmiHeader.biHeight = img->height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    if (g_isNT) {                       /* HALFTONE is NT-only; 9x falls back to COLORONCOLOR */
        SetStretchBltMode(di->hDC, HALFTONE);
        SetBrushOrgEx(di->hDC, 0, 0, NULL);
    } else {
        SetStretchBltMode(di->hDC, COLORONCOLOR);
    }
    StretchDIBits(di->hDC, x, y, dw, dh, 0, 0, img->width, img->height,
                  img->bits, &bi, DIB_RGB_COLORS, SRCCOPY);
    SetRect(&f, x - 1, y - 1, x + dw + 1, y + dh + 1);
    FrameRect(di->hDC, &f, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

static void SaveShot(HWND dlg, SHOT_CTX *sc)
{
    OPENFILENAME ofn;
    char path[MAX_PATH], base[128], filter[160];
    int fmt = SelectedFormat(dlg), rc;
    LS_BUF b;
    HCURSOR old;

    GetDlgItemText(dlg, IDC_NAME, base, sizeof(base));
    SanitizeName(base, g_formatExt[fmt], path);
    sprintf(filter, "%s image (*%s)|*%s|All files (*.*)|*.*|", g_formatNames[fmt],
            g_formatExt[fmt], g_formatExt[fmt]);
    {
        size_t i, n = strlen(filter);   /* '|' -> NUL; the list ends with a double NUL */
        for (i = 0; i < n; i++) if (filter[i] == '|') filter[i] = '\0';
    }
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400;     /* Windows 95 / NT 4 reject the larger struct */
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrDefExt = g_formatExt[fmt] + 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetSaveFileName(&ofn))
        return;

    memset(&b, 0, sizeof(b));
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    rc = ImgEncode(fmt, sc->img, &b, NULL, NULL, NULL);
    SetCursor(old);
    if (rc != IMG_OK) {
        MessageBox(dlg, ImgErrorText(rc), APP_NAME, MB_ICONEXCLAMATION);
    } else if (!WriteWholeFile(path, b.data, b.size)) {
        MessageBox(dlg, "The file could not be written.", APP_NAME, MB_ICONEXCLAMATION);
    } else {
        char sz[32], t[MAX_PATH + 64];
        FormatBytes(b.size, sz);
        _snprintf(t, sizeof(t) - 1, "Saved %s (%s).", path, sz);
        t[sizeof(t) - 1] = '\0';
        SetDlgItemText(dlg, IDC_INFO, t);
    }
    BufFree(&b);
}

static void UploadShot(HWND dlg, SHOT_CTX *sc)
{
    UPLOAD_JOB *j;
    char base[128], desc[1024];

    if (!SettingsComplete()) {
        MessageBox(dlg, "Enter your LegacyShot server, user name and API key first.",
                   APP_NAME, MB_ICONINFORMATION);
        if (!RunSettings(dlg) || !SettingsComplete())
            return;
    }
    j = (UPLOAD_JOB *)calloc(1, sizeof(UPLOAD_JOB));
    if (j == NULL)
        return;
    j->img = sc->img;
    j->format = SelectedFormat(dlg);
    GetDlgItemText(dlg, IDC_NAME, base, sizeof(base));
    SanitizeName(base, g_formatExt[j->format], j->name);
    GetDlgItemText(dlg, IDC_DESC, desc, sizeof(desc));
    ToCp1252(desc, j->desc, sizeof(j->desc));

    if (j->format != g_set.format) {    /* remember the last format used */
        g_set.format = j->format;
        WriteInt("Options", "Format", g_set.format);
    }
    if (RunUpload(dlg, j))
        EndDialog(dlg, SHOT_UPLOADED);
    free(j);
}

static BOOL CALLBACK ShotDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SHOT_CTX *sc = (SHOT_CTX *)GetWindowLong(dlg, DWL_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        char name[64], t[80];
        sc = (SHOT_CTX *)lp;
        SetWindowLong(dlg, DWL_USER, (LONG)sc);
        DefaultName(name);
        SetDlgItemText(dlg, IDC_NAME, name);
        SendDlgItemMessage(dlg, IDC_NAME, EM_LIMITTEXT, 90, 0);
        SendDlgItemMessage(dlg, IDC_DESC, EM_LIMITTEXT, 1000, 0);
        CheckRadioButton(dlg, IDC_FMT_PNG, IDC_FMT_BMP, IDC_FMT_PNG + g_set.format);
        if (sc->colors <= 256)
            sprintf(t, "%d x %d pixels, %d colors", sc->img->width, sc->img->height, sc->colors);
        else
            sprintf(t, "%d x %d pixels", sc->img->width, sc->img->height);
        SetDlgItemText(dlg, IDC_INFO, t);
        UpdateHint(dlg, sc);
        SendMessage(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(g_inst, MAKEINTRESOURCE(IDI_APP)));
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_UPLOAD));
        return FALSE;                   /* focus set by hand */
    }

    case WM_DRAWITEM:
        if (wp == IDC_PREVIEW && sc != NULL) {
            DrawPreview((const DRAWITEMSTRUCT *)lp, sc->img);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FMT_PNG:
        case IDC_FMT_GIF:
        case IDC_FMT_BMP:
            UpdateHint(dlg, sc);
            return TRUE;
        case IDC_UPLOAD:
            UploadShot(dlg, sc);
            return TRUE;
        case IDC_SAVE:
            SaveShot(dlg, sc);
            return TRUE;
        case IDC_COPY:
            SetDlgItemText(dlg, IDC_INFO, CopyImageToClipboard(dlg, sc->img)
                           ? "Copied to the clipboard." : "Could not copy to the clipboard.");
			EndDialog(dlg, SHOT_CLOSE);
            return TRUE;
        case IDC_RESELECT:
            EndDialog(dlg, SHOT_RESELECT);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, SHOT_CLOSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ====================================================================== capture flow */

static void DoCapture(int whole)
{
    LS_IMAGE screen, shot;
    POINT origin;
    RECT sel;
    SHOT_CTX sc;
    int r;

    if (g_busy) {
        HWND pop = GetLastActivePopup(g_main);
        if (pop != NULL && pop != g_main) SetForegroundWindow(pop);
        else MessageBeep(MB_OK);
        return;
    }
    g_busy = 1;
    memset(&screen, 0, sizeof(screen));
    if (!CaptureScreen(&screen, &origin)) {
        MessageBox(NULL, "The screen could not be captured (not enough memory?).", APP_NAME, MB_ICONEXCLAMATION);
        g_busy = 0;
        return;
    }
    for (;;) {
        if (whole)
            SetRect(&sel, 0, 0, screen.width, screen.height);
        else if (!SelectRegion(g_inst, &screen, origin, &sel))
            break;
        memset(&shot, 0, sizeof(shot));
        if (!ImgCrop(&screen, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top, &shot)) {
            MessageBox(NULL, "Not enough memory for the screenshot.", APP_NAME, MB_ICONEXCLAMATION);
            break;
        }
        sc.img = &shot;
        sc.colors = ImgCountColors(&shot);
        r = (int)DialogBoxParam(g_inst, MAKEINTRESOURCE(IDD_SHOT), g_main, (DLGPROC)ShotDlgProc, (LPARAM)&sc);
        ImgFree(&shot);
        if (r != SHOT_RESELECT)
            break;
        whole = 0;                      /* Reselect always goes back to the overlay */
    }
    ImgFree(&screen);
    g_busy = 0;
}

static int DetectImageType(const unsigned char *p, unsigned long n)
{
    if (n >= 4 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return LSUP_IMG_PNG;
    if (n >= 4 && memcmp(p, "GIF8", 4) == 0) return LSUP_IMG_GIF;
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return LSUP_IMG_JPEG;
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return LSUP_IMG_BMP;
    return 0;
}

static void UploadFile(void)
{
    OPENFILENAME ofn;
    char path[MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    unsigned char *data;
    UPLOAD_JOB *j;
    const char *base;

    if (g_busy) { DoCapture(0); return; }  /* just brings the open dialog forward */
    path[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "Images (*.png;*.gif;*.bmp;*.jpg)\0*.png;*.gif;*.bmp;*.jpg;*.jpeg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    g_busy = 1;
    if (!GetOpenFileName(&ofn)) {
        g_busy = 0;
        return;
    }
    if (!SettingsComplete() && (!RunSettings(g_main) || !SettingsComplete())) {
        g_busy = 0;
        return;
    }
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, "The file could not be opened.", APP_NAME, MB_ICONEXCLAMATION);
        g_busy = 0;
        return;
    }
    size = GetFileSize(h, NULL);
    data = (size > 0 && size <= MAX_FILE_UPLOAD) ? (unsigned char *)malloc(size) : NULL;
    if (data == NULL || !ReadFile(h, data, size, &got, NULL) || got != size) {
        CloseHandle(h);
        if (data) free(data);
        MessageBox(NULL, "The file is empty, too large (64 MB limit) or could not be read.",
                   APP_NAME, MB_ICONEXCLAMATION);
        g_busy = 0;
        return;
    }
    CloseHandle(h);

    j = (UPLOAD_JOB *)calloc(1, sizeof(UPLOAD_JOB));
    if (j != NULL) {
        j->imageType = DetectImageType(data, size);
        if (j->imageType == 0) {
            MessageBox(NULL, "That is not a PNG, GIF, BMP or JPEG image.", APP_NAME, MB_ICONEXCLAMATION);
        } else {
            j->fileData = data;
            j->fileSize = size;
            base = strrchr(path, '\\');
            base = base ? base + 1 : path;
            SanitizeName(base, NULL, j->name);
            RunUpload(g_main, j);
        }
        free(j);
    }
    free(data);
    g_busy = 0;
}

/* ====================================================================== tray + hotkeys */

/* Returns 0 if a hotkey that is switched on could not be registered. */
static int RegisterHotkeys(void)
{
    int ok = 1;
    UnregisterHotKey(g_main, HOTKEY_PRTSC);
    UnregisterHotKey(g_main, HOTKEY_OTHER);
    if (g_set.usePrtSc && !RegisterHotKey(g_main, HOTKEY_PRTSC, 0, VK_SNAPSHOT))
        ok = 0;
    if (LOBYTE(g_set.hotkey)) {
        UINT mods = 0;
        if (HIBYTE(g_set.hotkey) & HOTKEYF_SHIFT)   mods |= MOD_SHIFT;
        if (HIBYTE(g_set.hotkey) & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
        if (HIBYTE(g_set.hotkey) & HOTKEYF_ALT)     mods |= MOD_ALT;
        if (!RegisterHotKey(g_main, HOTKEY_OTHER, mods, LOBYTE(g_set.hotkey)))
            ok = 0;
    }
    return ok;
}

static void TrayAdd(int hotkeysOk)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = NOTIFYICONDATA_V1_SIZE;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImage(g_inst, MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, 16, 16, 0);
    strcpy(g_nid.szTip, (hotkeysOk && g_set.usePrtSc)
                        ? "LegacyShot - Print Screen or click to capture"
                        : "LegacyShot - click to capture");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void ShowTrayMenu(void)
{
    HMENU menu = LoadMenu(g_inst, MAKEINTRESOURCE(IDR_TRAYMENU)), sub;
    POINT pt;
    if (menu == NULL)
        return;
    sub = GetSubMenu(menu, 0);
    SetMenuDefaultItem(sub, ID_CAPTURE_REGION, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);        /* so the menu closes when clicking elsewhere (Q135788, thanks Microsoft) */
    TrackPopupMenu(sub, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    PostMessage(g_main, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {    /* Explorer restarted */
        TrayAdd(1);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) {
            /* Take the foreground while the shell still allows it, so the
             * overlay gets the keyboard (Esc, Space) on 98/2000 and later. */
            SetForegroundWindow(hwnd);
            SetTimer(hwnd, TIMER_CAP_AREA, 50, NULL);
        }
        else if (lp == WM_RBUTTONUP)
            ShowTrayMenu();
        return 0;

    case WM_HOTKEY:
        DoCapture(0);                   /* capture at once: freezes menus and tooltips too */
        return 0;

    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (wp == TIMER_CAP_AREA || wp == TIMER_CAP_FULL)
            DoCapture(wp == TIMER_CAP_FULL);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CAPTURE_REGION:         /* wait for the menu to vanish from the screen */
            SetTimer(hwnd, TIMER_CAP_AREA, 300, NULL);
            break;
        case ID_CAPTURE_FULL:
            SetTimer(hwnd, TIMER_CAP_FULL, 300, NULL);
            break;
        case ID_UPLOAD_FILE:
            UploadFile();
            break;
        case ID_SETTINGS:
            if (g_busy) { DoCapture(0); break; }
            g_busy = 1;
            RunSettings(NULL);
            g_busy = 0;
            break;
        case ID_ABOUT:
            MessageBox(NULL,
                "LegacyShot 1.0 for Windows 95 / NT 4 \n"
				"Presented by nick99nack\n\n"
                "Press Print Screen or click the tray icon, then drag an area or click a window.\n",
                "About LegacyShot", MB_ICONINFORMATION);
            break;
        case ID_EXIT:
            if (g_busy) { DoCapture(0); break; }
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_PRTSC);
        UnregisterHotKey(hwnd, HOTKEY_OTHER);
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ====================================================================== WinMain */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    WSADATA wsa;
    MSG msg;
    HANDLE mutex;
    OSVERSIONINFO ov;
    int hotkeysOk;
    typedef BOOL (WINAPI *SETDPIAWARE)(void);
    SETDPIAWARE setDpiAware;

    (void)prev;
    (void)show;
    g_inst = inst;

    /* Without this, scaled displays (Vista+) capture a blurry, cropped desktop. */
    setDpiAware = (SETDPIAWARE)GetProcAddress(GetModuleHandle("user32.dll"), "SetProcessDPIAware");
    if (setDpiAware != NULL)
        setDpiAware();

    /* One copy only: a second start triggers a capture in the running one. */
    mutex = CreateMutex(NULL, FALSE, MUTEX_NAME);
    if (mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindow(MAIN_CLASS, NULL);
        if (other != NULL) {
            UINT id = ID_CAPTURE_REGION;
            if (strstr(cmd, "/full") || strstr(cmd, "/FULL")) id = ID_CAPTURE_FULL;
            if (strstr(cmd, "/settings") || strstr(cmd, "/SETTINGS")) id = ID_SETTINGS;
            PostMessage(other, WM_COMMAND, id, 0);
        }
        return 0;
    }

    ov.dwOSVersionInfoSize = sizeof(ov);
    g_isNT = GetVersionEx(&ov) && ov.dwPlatformId == VER_PLATFORM_WIN32_NT;

    InitCommonControls();
    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        MessageBox(NULL, "Windows Sockets could not be started. Is TCP/IP installed?",
                   APP_NAME, MB_ICONSTOP);
        return 1;
    }
    LoadSettings();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APP));
    wc.lpszClassName = MAIN_CLASS;
    RegisterClass(&wc);
    g_main = CreateWindow(MAIN_CLASS, APP_NAME, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (g_main == NULL)
        return 1;

    g_taskbarCreated = RegisterWindowMessage("TaskbarCreated");
    hotkeysOk = RegisterHotkeys();
    TrayAdd(hotkeysOk);

    if (!IniExists())
        PostMessage(g_main, WM_COMMAND, ID_SETTINGS, 0);
    else if (!hotkeysOk)
        MessageBox(NULL, "Print Screen is already used by another program, so LegacyShot "
                   "cannot use it. Click the LegacyShot tray icon to capture, or pick "
                   "another hotkey in Settings.", APP_NAME, MB_ICONINFORMATION);
    if (strstr(cmd, "/full") || strstr(cmd, "/FULL"))
        PostMessage(g_main, WM_COMMAND, ID_CAPTURE_FULL, 0);
    else if (strstr(cmd, "/capture") || strstr(cmd, "/CAPTURE"))
        PostMessage(g_main, WM_COMMAND, ID_CAPTURE_REGION, 0);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    WSACleanup();
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
