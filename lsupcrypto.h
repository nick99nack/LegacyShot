/*
 * lsupcrypto.h - MD5, HMAC-MD5 and CRC-32 for LegacyShot.
 * Plain C89, no CryptoAPI: runs on Windows 95 without OSR2.
 */
#ifndef LSUPCRYPTO_H
#define LSUPCRYPTO_H

typedef unsigned int  lsup_u32;   /* 32 bits on VC++ 6.0 and on every gcc target */
typedef unsigned char lsup_u8;

typedef struct {
    lsup_u32 state[4];
    lsup_u32 bytesLo;             /* message length in bytes, 64-bit as two halves */
    lsup_u32 bytesHi;
    lsup_u8  block[64];
} LSUP_MD5;

void LsupMd5Init(LSUP_MD5 *ctx);
void LsupMd5Update(LSUP_MD5 *ctx, const void *data, unsigned long len);
void LsupMd5Final(LSUP_MD5 *ctx, lsup_u8 digest[16]);
void LsupMd5(const void *data, unsigned long len, lsup_u8 digest[16]);

/* HMAC-MD5 (RFC 2104) over the concatenation of up to 4 parts; unused parts are NULL/0. */
void LsupHmacMd5(const lsup_u8 *key, unsigned long keyLen,
                 const void *p1, unsigned long n1, const void *p2, unsigned long n2,
                 const void *p3, unsigned long n3, const void *p4, unsigned long n4,
                 lsup_u8 mac[16]);

/* CRC-32 (IEEE 802.3, as used by LSUP frames, PNG chunks and zip).
 * crc = LsupCrcFinish(LsupCrcUpdate(LsupCrcUpdate(LSUP_CRC_INIT, a, n), b, m)); */
#define LSUP_CRC_INIT 0xFFFFFFFFUL
lsup_u32 LsupCrcUpdate(lsup_u32 crc, const void *data, unsigned long len);
#define LsupCrcFinish(c) ((lsup_u32)((c) ^ 0xFFFFFFFFUL))

#endif
