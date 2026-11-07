/*
 * lsup.h - LSUP/2 protocol client (blocking sockets; call it from a worker thread).
 * Plain C89 + Winsock 1.1, so it builds with VC++ 6.0 and runs on Windows 95 / NT 4.
 * Also builds on POSIX (gcc) for the tests in test/.
 */
#ifndef LSUP_H
#define LSUP_H

#ifdef _WIN32
#include <winsock.h>
typedef SOCKET lsup_socket;
#else
typedef int lsup_socket;
#endif

#include "lsupcrypto.h"

/* Return codes: 0 = OK, negative = local problem, positive = LSUP/2 server error code. */
#define LSUP_OK            0
#define LSUP_E_NET        (-1)   /* connect failed, line dropped, timeout: worth retrying */
#define LSUP_E_PROTO      (-2)   /* server sent something unexpected */
#define LSUP_E_CANCELLED  (-3)
#define LSUP_E_ARG        (-4)   /* bad argument: empty image, file too big for the server */
#define LSUP_E_BADSERVER  (-5)   /* ServerProof did not verify: not the real server */

/* Server error codes the client logic branches on (spec, "Error codes"). */
#define LSUP_ERR_BADCRC       0x0002
#define LSUP_ERR_TIMEOUT      0x0006
#define LSUP_ERR_AUTHFAILED   0x0010
#define LSUP_ERR_LOCKED       0x0011
#define LSUP_ERR_BADTOKEN     0x0012
#define LSUP_ERR_BADMAC       0x0013
#define LSUP_ERR_BADUPLOADID  0x0100
#define LSUP_ERR_BADCHUNK     0x0101
#define LSUP_ERR_TOOLARGE     0x0102
#define LSUP_ERR_MD5MISMATCH  0x0103
#define LSUP_ERR_SERVERBUSY   0x0200
#define LSUP_ERR_INTERNAL     0x0201

/* ImageType in UPLOAD_BEGIN */
#define LSUP_IMG_BMP  1
#define LSUP_IMG_JPEG 2
#define LSUP_IMG_GIF  3
#define LSUP_IMG_PNG  4

/* Progress phases */
#define LSUP_PH_START   1        /* done = bytes already on the server (resume point), total = size */
#define LSUP_PH_SEND    2        /* done = bytes acknowledged by the server, total = size */

typedef void (*LSUP_PROGRESS_FN)(void *ctx, int phase, unsigned long done, unsigned long total);

#define LSUP_PORT        7745
#define LSUP_MAX_PAYLOAD 65600UL
#define LSUP_FRAME_MAX   (LSUP_MAX_PAYLOAD + 11)

typedef struct {
    lsup_socket sock;
    int connected;
    int timeoutSec;                     /* per-ack timeout from the link profile (60/30/20) */
    volatile long *cancel;              /* set *cancel != 0 from another thread to stop */

    lsup_u8 token[16];
    lsup_u8 sessionKey[16];
    unsigned long minChunk, maxChunk, maxFileSize;
    int maxWindow;
    int loggedIn;

    char err[256];                      /* last error, human readable */

    unsigned char rbuf[LSUP_MAX_PAYLOAD];  /* payload of the last frame received */
    unsigned long rlen;
    unsigned char sbuf[LSUP_FRAME_MAX];    /* frame being sent */
    unsigned char pbuf[LSUP_MAX_PAYLOAD];  /* payload being built */
} LSUP_CONN;

/* Windows: the caller does WSAStartup(MAKEWORD(1,1)) once per process. */
LSUP_CONN *LsupNew(void);
void LsupFree(LSUP_CONN *c);           /* closes the socket if still open */

int  LsupConnect(LSUP_CONN *c, const char *host, unsigned short port);
int  LsupLogin(LSUP_CONN *c, const char *user, const char *apiKey);

/*
 * Uploads one image held in memory. md5 is the MD5 of data (computed once by
 * the caller so retries do not rehash). Resumes automatically when the server
 * holds part of the same file (same user, MD5 and size). requestedChunk and
 * window come from the link profile; the server may clamp both.
 * On success the server's reference (URL or file ID) is copied to reference.
 */
int  LsupUpload(LSUP_CONN *c, const unsigned char *data, unsigned long size,
                const lsup_u8 md5[16], int imageType,
                const char *fileName, const char *description,
                unsigned long requestedChunk, int window,
                LSUP_PROGRESS_FN progress, void *progressCtx,
                char *reference, int referenceSize);

void LsupLogout(LSUP_CONN *c);         /* sends LOGOUT (best effort) and closes */
void LsupClose(LSUP_CONN *c);

/* Name of a code ("AuthFailed", "Md5Mismatch", ...) for logs. */
const char *LsupErrorName(int code);

/* 1 if trying again (after reconnecting) can help with this result code. */
int  LsupIsRetryable(int code);

#endif
