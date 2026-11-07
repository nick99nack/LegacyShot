/*
 * lsup.c - LSUP/2 protocol client. See lsup.h and the LSUP/2 specification.
 *
 * Blocking sockets with select() timeouts, polled every 250 ms so a Cancel
 * from the GUI thread takes effect quickly even on a stalled modem.
 * Fields are serialized byte by byte (never packed structs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock.h>
#define SOCK_ERRNO()      WSAGetLastError()
#define SOCK_INPROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#if defined(_MSC_VER) && _MSC_VER < 1900
#define vsnprintf         _vsnprintf     /* VC++ 6.0 only has the underscore name */
#endif
typedef int lsup_socklen;
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#define INVALID_SOCKET    (-1)
#define SOCKET_ERROR      (-1)
#define closesocket       close
#define ioctlsocket       ioctl
#define SOCK_ERRNO()      errno
#define SOCK_INPROGRESS(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK)
typedef socklen_t lsup_socklen;
#endif

#include "lsup.h"

/* Message types (spec, "Message catalog") */
#define T_HELLO            0x01
#define T_CHALLENGE        0x02
#define T_LOGIN            0x03
#define T_LOGIN_OK         0x04
#define T_LOGIN_FAIL       0x05
#define T_UPLOAD_BEGIN     0x10
#define T_UPLOAD_BEGIN_ACK 0x11
#define T_CHUNK            0x12
#define T_CHUNK_ACK        0x13
#define T_UPLOAD_END       0x14
#define T_UPLOAD_RESULT    0x15
#define T_UPLOAD_ABORT     0x16
#define T_PING             0x20
#define T_PONG             0x21
#define T_LOGOUT           0x30
#define T_ERROR            0x7F

#define CLIENT_KIND_VCPP   2
#define CLIENT_BUILD       100
#define CLIENT_NAME        "LegacyShot VC++ 1.0"

#define POLL_MS            250
#define CONNECT_TIMEOUT_S  30

/* ------------------------------------------------------------------ helpers */

static void SetErr(LSUP_CONN *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err) - 1, fmt, ap);
    c->err[sizeof(c->err) - 1] = '\0';
    va_end(ap);
}

static void PutU16(unsigned char *p, unsigned long v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void PutU32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned long GetU16(const unsigned char *p) { return (unsigned long)p[0] | ((unsigned long)p[1] << 8); }
static unsigned long GetU32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int Cancelled(LSUP_CONN *c) { return c->cancel != NULL && *c->cancel != 0; }

const char *LsupErrorName(int code)
{
    switch (code) {
    case LSUP_OK:          return "OK";
    case LSUP_E_NET:       return "NetworkError";
    case LSUP_E_PROTO:     return "ProtocolError";
    case LSUP_E_CANCELLED: return "Cancelled";
    case LSUP_E_ARG:       return "BadArgument";
    case LSUP_E_BADSERVER: return "BadServerProof";
    case 0x0001: return "BadMagic";
    case 0x0002: return "BadCrc";
    case 0x0003: return "UnknownType";
    case 0x0004: return "FrameTooLarge";
    case 0x0005: return "BadVersion";
    case 0x0006: return "Timeout";
    case 0x0007: return "MalformedPayload";
    case 0x0010: return "AuthFailed";
    case 0x0011: return "Locked";
    case 0x0012: return "BadToken";
    case 0x0013: return "BadMac";
    case 0x0100: return "BadUploadId";
    case 0x0101: return "BadChunk";
    case 0x0102: return "TooLarge";
    case 0x0103: return "Md5Mismatch";
    case 0x0104: return "BadFileName";
    case 0x0105: return "BadImageType";
    case 0x0106: return "QuotaExceeded";
    case 0x0200: return "ServerBusy";
    case 0x0201: return "InternalError";
    default:     return "Unknown";
    }
}

int LsupIsRetryable(int code)
{
    switch (code) {
    case LSUP_E_NET:
    case 0x0001: case 0x0002: case 0x0006:      /* BadMagic, BadCrc, Timeout */
    case LSUP_ERR_BADTOKEN: case LSUP_ERR_BADUPLOADID: case LSUP_ERR_BADCHUNK:
    case LSUP_ERR_MD5MISMATCH: case LSUP_ERR_SERVERBUSY: case LSUP_ERR_INTERNAL:
        return 1;
    default:
        return 0;
    }
}

static const char *ServerErrorText(unsigned long code)
{
    switch (code) {
    case 0x0005: return "The server does not support this client version.";
    case 0x0010: return "Wrong user name or API key.";
    case 0x0011: return "Too many failed logins. The account is locked for a while.";
    case 0x0012: return "The server rejected the session. Log in again.";
    case 0x0013: return "The server rejected a message signature (BadMac).";
    case 0x0101: return "A chunk arrived out of order. Reconnecting will resume.";
    case 0x0102: return "The image is larger than the server allows.";
    case 0x0103: return "The image was damaged in transit. It will be sent again.";
    case 0x0104: return "The server rejected the file name.";
    case 0x0105: return "The server rejected the image type.";
    case 0x0106: return "The server is out of space or over quota.";
    case 0x0200: return "The server is busy. Try again in a minute.";
    case 0x0201: return "The server had an internal error. Try again later.";
    default:     return NULL;
    }
}

/* ------------------------------------------------------------------ socket I/O */

/* Waits until the socket is readable (forWrite = 0) or writable (1).
 * Returns LSUP_OK, LSUP_E_NET on timeout, LSUP_E_CANCELLED. */
static int WaitSock(LSUP_CONN *c, int forWrite, int timeoutSec)
{
    long waitedMs = 0, limitMs = (long)timeoutSec * 1000L;
    for (;;) {
        fd_set set;
        struct timeval tv;
        int n;

        if (Cancelled(c)) {
            SetErr(c, "Cancelled.");
            return LSUP_E_CANCELLED;
        }
        FD_ZERO(&set);
        FD_SET(c->sock, &set);
        tv.tv_sec = 0;
        tv.tv_usec = POLL_MS * 1000L;
        n = select((int)c->sock + 1, forWrite ? NULL : &set, forWrite ? &set : NULL, NULL, &tv);
        if (n > 0)
            return LSUP_OK;
        if (n < 0) {
            SetErr(c, "Network error (select failed, code %d).", SOCK_ERRNO());
            return LSUP_E_NET;
        }
        waitedMs += POLL_MS;
        if (waitedMs >= limitMs) {
            SetErr(c, "The server did not respond for %d seconds.", timeoutSec);
            return LSUP_E_NET;
        }
    }
}

static int SendAll(LSUP_CONN *c, const unsigned char *p, unsigned long len)
{
    while (len > 0) {
        int rc = WaitSock(c, 1, c->timeoutSec), n;
        if (rc != LSUP_OK)
            return rc;
        n = send(c->sock, (const char *)p, (int)(len > 32768UL ? 32768UL : len), 0);
        if (n == SOCKET_ERROR || n == 0) {
            SetErr(c, "The connection was lost while sending (code %d).", SOCK_ERRNO());
            return LSUP_E_NET;
        }
        p += n;
        len -= (unsigned long)n;
    }
    return LSUP_OK;
}

static int RecvAll(LSUP_CONN *c, unsigned char *p, unsigned long len, int timeoutSec)
{
    while (len > 0) {
        int rc = WaitSock(c, 0, timeoutSec), n;
        if (rc != LSUP_OK)
            return rc;
        n = recv(c->sock, (char *)p, (int)(len > 32768UL ? 32768UL : len), 0);
        if (n == 0) {
            SetErr(c, "The server closed the connection.");
            return LSUP_E_NET;
        }
        if (n == SOCKET_ERROR) {
            SetErr(c, "The connection was lost (code %d).", SOCK_ERRNO());
            return LSUP_E_NET;
        }
        p += n;
        len -= (unsigned long)n;
    }
    return LSUP_OK;
}

/* ------------------------------------------------------------------ framing */

static int SendFrame(LSUP_CONN *c, unsigned char type, const unsigned char *payload, unsigned long len)
{
    lsup_u32 crc;
    if (len > LSUP_MAX_PAYLOAD)
        return LSUP_E_ARG;
    c->sbuf[0] = 0xAB;
    c->sbuf[1] = 0xCD;
    c->sbuf[2] = type;
    PutU32(c->sbuf + 3, len);
    if (len)
        memcpy(c->sbuf + 7, payload, len);
    crc = LsupCrcFinish(LsupCrcUpdate(LSUP_CRC_INIT, c->sbuf + 2, len + 5));
    PutU32(c->sbuf + 7 + len, crc);
    return SendAll(c, c->sbuf, len + 11);
}

/*
 * Receives the next frame into c->rbuf / c->rlen. Answers PINGs, skips PONGs.
 * Returns LSUP_OK, a negative local error, or a positive server error code if
 * an ERROR frame arrived (UnknownType is ignored, as the spec says).
 */
static int RecvFrame(LSUP_CONN *c, unsigned char *type, int timeoutSec)
{
    unsigned char hdr[7], trailer[4];
    unsigned long len;
    lsup_u32 crc;
    int rc;

    for (;;) {
        rc = RecvAll(c, hdr, 7, timeoutSec);
        if (rc != LSUP_OK)
            return rc;
        if (hdr[0] != 0xAB || hdr[1] != 0xCD) {
            SetErr(c, "The server sent data that is not LSUP/2.");
            return LSUP_E_PROTO;
        }
        len = GetU32(hdr + 3);
        if (len > LSUP_MAX_PAYLOAD) {
            SetErr(c, "The server sent an oversized frame.");
            return LSUP_E_PROTO;
        }
        if (len > 0) {
            rc = RecvAll(c, c->rbuf, len, timeoutSec);
            if (rc != LSUP_OK)
                return rc;
        }
        rc = RecvAll(c, trailer, 4, timeoutSec);
        if (rc != LSUP_OK)
            return rc;
        crc = LsupCrcFinish(LsupCrcUpdate(LsupCrcUpdate(LSUP_CRC_INIT, hdr + 2, 5), c->rbuf, len));
        if (crc != (lsup_u32)GetU32(trailer)) {
            SetErr(c, "Data from the server was corrupted (CRC mismatch).");
            return LSUP_E_NET;          /* reconnect and resume */
        }

        c->rlen = len;
        *type = hdr[2];

        if (*type == T_PING) {
            rc = SendFrame(c, T_PONG, NULL, 0);
            if (rc != LSUP_OK)
                return rc;
            continue;
        }
        if (*type == T_PONG)
            continue;

        if (*type == T_ERROR) {
            unsigned long code = (len >= 2) ? GetU16(c->rbuf) : 0;
            const char *text = ServerErrorText(code);
            if (code == 0x0003)         /* UnknownType: feature not supported, carry on */
                continue;
            if (text != NULL) {
                SetErr(c, "%s", text);
            } else {
                char msg[200];
                unsigned long n = (len >= 4) ? c->rbuf[3] : 0;
                if (len < 4 || n > len - 4) n = 0;
                if (n > sizeof(msg) - 1) n = sizeof(msg) - 1;
                memcpy(msg, c->rbuf + 4, n);
                msg[n] = '\0';
                SetErr(c, "Server error %s (0x%04lX). %s", LsupErrorName((int)code), code, msg);
            }
            return code ? (int)code : LSUP_E_PROTO;
        }
        return LSUP_OK;
    }
}

/* Receives a frame and checks it is `want` with at least minLen payload bytes. */
static int RecvExpect(LSUP_CONN *c, unsigned char want, unsigned long minLen, int timeoutSec)
{
    unsigned char type;
    int rc = RecvFrame(c, &type, timeoutSec);
    if (rc != LSUP_OK)
        return rc;
    if (type != want || c->rlen < minLen) {
        SetErr(c, "Unexpected reply from the server (type 0x%02X, %lu bytes; expected 0x%02X).",
               type, c->rlen, want);
        return LSUP_E_PROTO;
    }
    return LSUP_OK;
}

/* ------------------------------------------------------------------ nonce */

/* ClientNonce = MD5(tick count, high-resolution counter, time, process id,
 * cursor position, previous nonce). Needs to be unique, not secret. */
static void MakeNonce(lsup_u8 out[16])
{
    static lsup_u8 prev[16];
    static unsigned long counter = 0;
    LSUP_MD5 m;
    unsigned long v;
    time_t now = time(NULL);
    clock_t ck = clock();

    LsupMd5Init(&m);
    LsupMd5Update(&m, prev, 16);
    counter++;
    LsupMd5Update(&m, &counter, sizeof(counter));
    LsupMd5Update(&m, &now, sizeof(now));
    LsupMd5Update(&m, &ck, sizeof(ck));
#ifdef _WIN32
    {
        LARGE_INTEGER qpc;
        POINT pt;
        v = GetTickCount();
        LsupMd5Update(&m, &v, sizeof(v));
        if (QueryPerformanceCounter(&qpc))
            LsupMd5Update(&m, &qpc, sizeof(qpc));
        v = GetCurrentProcessId();
        LsupMd5Update(&m, &v, sizeof(v));
        v = GetCurrentThreadId();
        LsupMd5Update(&m, &v, sizeof(v));
        if (GetCursorPos(&pt))
            LsupMd5Update(&m, &pt, sizeof(pt));
    }
#else
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        LsupMd5Update(&m, &tv, sizeof(tv));
        v = (unsigned long)getpid();
        LsupMd5Update(&m, &v, sizeof(v));
    }
#endif
    v = (unsigned long)(size_t)out;          /* stack address adds a little more */
    LsupMd5Update(&m, &v, sizeof(v));
    LsupMd5Final(&m, out);
    memcpy(prev, out, 16);
}

/* ------------------------------------------------------------------ public API */

LSUP_CONN *LsupNew(void)
{
    LSUP_CONN *c = (LSUP_CONN *)malloc(sizeof(LSUP_CONN));
    if (c == NULL)
        return NULL;
    memset(c, 0, sizeof(*c));
    c->sock = INVALID_SOCKET;
    c->timeoutSec = 30;
    return c;
}

void LsupClose(LSUP_CONN *c)
{
    if (c->sock != INVALID_SOCKET) {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
    c->connected = 0;
    c->loggedIn = 0;
    memset(c->sessionKey, 0, sizeof(c->sessionKey));
}

void LsupFree(LSUP_CONN *c)
{
    if (c == NULL)
        return;
    LsupClose(c);
    free(c);
}

int LsupConnect(LSUP_CONN *c, const char *host, unsigned short port)
{
    struct sockaddr_in sa;
    unsigned long addr, nb;
    int rc, err, waited = 0;

    LsupClose(c);
    c->err[0] = '\0';

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    addr = inet_addr(host);
    if (addr == (unsigned long)INADDR_NONE || addr == 0xFFFFFFFFUL) {
        struct hostent *he = gethostbyname(host);   /* blocks; Winsock 1.1 has nothing better */
        if (he == NULL || he->h_addr_list[0] == NULL) {
            SetErr(c, "Cannot find the server \"%.100s\".", host);
            return LSUP_E_NET;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    } else {
        sa.sin_addr.s_addr = (unsigned int)addr;
    }
    if (Cancelled(c)) {
        SetErr(c, "Cancelled.");
        return LSUP_E_CANCELLED;
    }

    c->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (c->sock == INVALID_SOCKET) {
        SetErr(c, "Cannot create a socket (code %d). Is TCP/IP installed?", SOCK_ERRNO());
        return LSUP_E_NET;
    }

    /* Non-blocking connect so Cancel and our own timeout work (a blocking
     * connect on Windows 95 can hang for over a minute). */
    nb = 1;
    ioctlsocket(c->sock, FIONBIO, &nb);
    rc = connect(c->sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == SOCKET_ERROR) {
        err = SOCK_ERRNO();
        if (!SOCK_INPROGRESS(err)) {
            SetErr(c, "Cannot connect to %.100s:%u (code %d).", host, (unsigned)port, err);
            LsupClose(c);
            return LSUP_E_NET;
        }
        for (;;) {
            fd_set wset, eset;
            struct timeval tv;
            int n;
            if (Cancelled(c)) {
                SetErr(c, "Cancelled.");
                LsupClose(c);
                return LSUP_E_CANCELLED;
            }
            FD_ZERO(&wset); FD_SET(c->sock, &wset);
            FD_ZERO(&eset); FD_SET(c->sock, &eset);
            tv.tv_sec = 0;
            tv.tv_usec = POLL_MS * 1000L;
            n = select((int)c->sock + 1, NULL, &wset, &eset, &tv);
            if (n > 0) {
                int soerr = 0;
                lsup_socklen sl = sizeof(soerr);
                getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
                if (FD_ISSET(c->sock, &eset) || soerr != 0) {
                    SetErr(c, "The server %.100s:%u refused the connection or is unreachable (code %d).",
                           host, (unsigned)port, soerr);
                    LsupClose(c);
                    return LSUP_E_NET;
                }
                break;
            }
            waited += POLL_MS;
            if (n < 0 || waited >= CONNECT_TIMEOUT_S * 1000) {
                SetErr(c, "Timed out connecting to %.100s:%u.", host, (unsigned)port);
                LsupClose(c);
                return LSUP_E_NET;
            }
        }
    }
    nb = 0;
    ioctlsocket(c->sock, FIONBIO, &nb);
    {
        int one = 1;
        setsockopt(c->sock, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof(one));
    }
    c->connected = 1;
    return LSUP_OK;
}

int LsupLogin(LSUP_CONN *c, const char *user, const char *apiKey)
{
    unsigned char *p = c->pbuf;
    unsigned char msg[11 + 32 + 1 + 255];
    lsup_u8 serverNonce[16], clientNonce[16], response[16], proof[16];
    unsigned long userLen = (unsigned long)strlen(user), keyLen = (unsigned long)strlen(apiKey), n;
    int rc;

    if (!c->connected) {
        SetErr(c, "Not connected.");
        return LSUP_E_NET;
    }
    if (userLen == 0 || userLen > 255 || keyLen == 0) {
        SetErr(c, "Enter a user name (up to 255 characters) and an API key.");
        return LSUP_E_ARG;
    }

    /* HELLO */
    n = sizeof(CLIENT_NAME) - 1;
    p[0] = 2;                           /* ProtoVersion */
    p[1] = CLIENT_KIND_VCPP;
    PutU16(p + 2, CLIENT_BUILD);
    p[4] = (unsigned char)n;
    memcpy(p + 5, CLIENT_NAME, n);
    rc = SendFrame(c, T_HELLO, p, 5 + n);
    if (rc != LSUP_OK) return rc;

    /* CHALLENGE: B16 ServerNonce, U32 Min, U32 Max, U32 MaxFileSize, U8 MaxWindow */
    rc = RecvExpect(c, T_CHALLENGE, 29, c->timeoutSec);
    if (rc != LSUP_OK) return rc;
    memcpy(serverNonce, c->rbuf, 16);
    c->minChunk    = GetU32(c->rbuf + 16);
    c->maxChunk    = GetU32(c->rbuf + 20);
    c->maxFileSize = GetU32(c->rbuf + 24);
    c->maxWindow   = c->rbuf[28];
    if (c->maxWindow < 1) c->maxWindow = 1;

    /* LOGIN: STR8 Username, B16 ClientNonce, B16 Response */
    MakeNonce(clientNonce);
    memcpy(msg, "LSUP2 LOGIN", 11);
    memcpy(msg + 11, serverNonce, 16);
    memcpy(msg + 27, clientNonce, 16);
    msg[43] = (unsigned char)userLen;
    memcpy(msg + 44, user, userLen);
    LsupHmacMd5((const lsup_u8 *)apiKey, keyLen, msg, 44 + userLen, NULL, 0, NULL, 0, NULL, 0, response);

    p[0] = (unsigned char)userLen;
    memcpy(p + 1, user, userLen);
    memcpy(p + 1 + userLen, clientNonce, 16);
    memcpy(p + 17 + userLen, response, 16);
    rc = SendFrame(c, T_LOGIN, p, 33 + userLen);
    if (rc != LSUP_OK) return rc;

    {
        unsigned char type;
        rc = RecvFrame(c, &type, c->timeoutSec);
        if (rc != LSUP_OK) return rc;
        if (type == T_LOGIN_FAIL && c->rlen >= 4) {
            unsigned long code = GetU16(c->rbuf), retry = GetU16(c->rbuf + 2);
            if (code == LSUP_ERR_LOCKED)
                SetErr(c, "Too many failed logins. Try again in %lu minute(s).", (retry + 59) / 60);
            else
                SetErr(c, "%s", ServerErrorText(code) ? ServerErrorText(code) : "Login failed.");
            LsupClose(c);
            return code ? (int)code : LSUP_ERR_AUTHFAILED;
        }
        if (type != T_LOGIN_OK || c->rlen < 32) {
            SetErr(c, "Unexpected reply to LOGIN (type 0x%02X).", type);
            return LSUP_E_PROTO;
        }
    }

    /* LOGIN_OK: B16 SessionToken, B16 ServerProof. Check the proof before trusting anything. */
    memcpy(c->token, c->rbuf, 16);
    LsupHmacMd5((const lsup_u8 *)apiKey, keyLen, "LSUP2 SERVER", 12,
                clientNonce, 16, serverNonce, 16, NULL, 0, proof);
    if (memcmp(proof, c->rbuf + 16, 16) != 0) {
        SetErr(c, "The server could not prove it knows your API key. It may not be the real server.");
        LsupClose(c);
        return LSUP_E_BADSERVER;
    }
    LsupHmacMd5((const lsup_u8 *)apiKey, keyLen, "LSUP2 SKEY", 10,
                serverNonce, 16, clientNonce, 16, NULL, 0, c->sessionKey);
    c->loggedIn = 1;
    return LSUP_OK;
}

/* MAC = HMAC-MD5(SessionKey, Type || payload bytes before the MAC) */
static void Mac(LSUP_CONN *c, unsigned char type, const unsigned char *payload, unsigned long len, lsup_u8 out[16])
{
    LsupHmacMd5(c->sessionKey, 16, &type, 1, payload, len, NULL, 0, NULL, 0, out);
}

int LsupUpload(LSUP_CONN *c, const unsigned char *data, unsigned long size,
               const lsup_u8 md5[16], int imageType,
               const char *fileName, const char *description,
               unsigned long requestedChunk, int window,
               LSUP_PROGRESS_FN progress, void *progressCtx,
               char *reference, int referenceSize)
{
    unsigned char *p = c->pbuf;
    unsigned long nameLen = (unsigned long)strlen(fileName);
    unsigned long descLen = description ? (unsigned long)strlen(description) : 0;
    unsigned long n, uploadId, chunkSize, totalChunks, resumeFrom, next, acked;
    int rc, win;

    if (reference && referenceSize > 0)
        reference[0] = '\0';
    if (!c->loggedIn) {
        SetErr(c, "Not logged in.");
        return LSUP_E_NET;
    }
    if (size == 0 || size > 0x7FFFFFFFUL) {
        SetErr(c, "The image is empty or too large.");
        return LSUP_E_ARG;
    }
    if (c->maxFileSize && size > c->maxFileSize) {
        SetErr(c, "The image is %lu KB; the server accepts up to %lu KB. Try PNG or GIF.",
               (size + 1023) / 1024, c->maxFileSize / 1024);
        return LSUP_E_ARG;
    }
    if (nameLen > 255) nameLen = 255;
    if (descLen > 1000) descLen = 1000;

    /* UPLOAD_BEGIN */
    n = 0;
    memcpy(p + n, c->token, 16);            n += 16;
    memcpy(p + n, md5, 16);                 n += 16;
    PutU32(p + n, size);                    n += 4;
    PutU32(p + n, requestedChunk);          n += 4;
    p[n++] = (unsigned char)imageType;
    p[n++] = (unsigned char)nameLen;
    memcpy(p + n, fileName, nameLen);       n += nameLen;
    PutU16(p + n, descLen);                 n += 2;
    if (descLen) { memcpy(p + n, description, descLen); n += descLen; }
    Mac(c, T_UPLOAD_BEGIN, p, n, p + n);    n += 16;
    rc = SendFrame(c, T_UPLOAD_BEGIN, p, n);
    if (rc != LSUP_OK) return rc;

    /* UPLOAD_BEGIN_ACK: U32 UploadId, ChunkSize, TotalChunks, ResumeFromChunk, U8 Window */
    rc = RecvExpect(c, T_UPLOAD_BEGIN_ACK, 17, c->timeoutSec);
    if (rc != LSUP_OK) return rc;
    uploadId    = GetU32(c->rbuf);
    chunkSize   = GetU32(c->rbuf + 4);
    totalChunks = GetU32(c->rbuf + 8);
    resumeFrom  = GetU32(c->rbuf + 12);
    win         = c->rbuf[16];

    /* The server's values are authoritative; only refuse ones that cannot work. */
    if (chunkSize == 0 || chunkSize + 8 > LSUP_MAX_PAYLOAD || totalChunks == 0 ||
        (totalChunks - 1) > (size - 1) / chunkSize || totalChunks < (size - 1) / chunkSize + 1 ||
        resumeFrom > totalChunks) {
        SetErr(c, "The server sent an impossible chunk plan (%lu x %lu bytes, resume at %lu).",
               totalChunks, chunkSize, resumeFrom);
        return LSUP_E_PROTO;
    }
    if (win < 1) win = 1;
    if (window >= 1 && win > window) win = window;

    if (progress) {
        unsigned long have = resumeFrom * chunkSize;
        progress(progressCtx, LSUP_PH_START, have > size ? size : have, size);
    }

    /* Windowed stream: keep up to `win` chunks unacknowledged. */
    next = acked = resumeFrom;
    while (acked < totalChunks) {
        while (next < totalChunks && next - acked < (unsigned long)win) {
            unsigned long off = next * chunkSize;
            unsigned long len = (size - off < chunkSize) ? size - off : chunkSize;
            PutU32(p, uploadId);
            PutU32(p + 4, next);
            memcpy(p + 8, data + off, len);
            rc = SendFrame(c, T_CHUNK, p, 8 + len);
            if (rc != LSUP_OK) return rc;
            next++;
        }

        rc = RecvExpect(c, T_CHUNK_ACK, 8, c->timeoutSec);
        if (rc != LSUP_OK) return rc;
        if (GetU32(c->rbuf) != uploadId) {
            SetErr(c, "The server acknowledged a different upload.");
            return LSUP_E_PROTO;
        }
        n = GetU32(c->rbuf + 4);            /* AckThrough: contiguous chunks stored */
        if (n < acked || n > next) {
            SetErr(c, "The server acknowledged chunk %lu, but %lu..%lu were outstanding.", n, acked, next);
            return LSUP_E_PROTO;
        }
        acked = n;
        if (progress) {
            unsigned long have = acked * chunkSize;
            progress(progressCtx, LSUP_PH_SEND, have > size ? size : have, size);
        }
    }

    /* UPLOAD_END: U32 UploadId, B16 FileMD5, B16 MAC */
    PutU32(p, uploadId);
    memcpy(p + 4, md5, 16);
    Mac(c, T_UPLOAD_END, p, 20, p + 20);
    rc = SendFrame(c, T_UPLOAD_END, p, 36);
    if (rc != LSUP_OK) return rc;

    /* UPLOAD_RESULT: U32 UploadId, U16 Status, STR16 ServerReference.
     * The server MD5s the whole file first, so allow extra time. */
    rc = RecvExpect(c, T_UPLOAD_RESULT, 8, c->timeoutSec + 60);
    if (rc != LSUP_OK) return rc;
    {
        unsigned long status = GetU16(c->rbuf + 4);
        unsigned long refLen = GetU16(c->rbuf + 6);
        if (refLen > c->rlen - 8) refLen = c->rlen - 8;
        if (status != 0) {
            const char *t = ServerErrorText(status);
            SetErr(c, "%s", t ? t : "The server did not store the image.");
            return (int)status;
        }
        if (reference && referenceSize > 0) {
            if (refLen > (unsigned long)referenceSize - 1) refLen = (unsigned long)referenceSize - 1;
            memcpy(reference, c->rbuf + 8, refLen);
            reference[refLen] = '\0';
        }
    }
    return LSUP_OK;
}

void LsupLogout(LSUP_CONN *c)
{
    if (c->connected) {
        int saved = c->timeoutSec;
        volatile long *savedCancel = c->cancel;
        c->cancel = NULL;               /* logout is quick and should not be cut short */
        c->timeoutSec = 5;
        SendFrame(c, T_LOGOUT, NULL, 0);
        c->timeoutSec = saved;
        c->cancel = savedCancel;
    }
    LsupClose(c);
}
