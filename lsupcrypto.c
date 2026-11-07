/*
 * lsupcrypto.c - MD5 (RFC 1321), HMAC-MD5 (RFC 2104) and CRC-32.
 * Plain C89. Tables are constant, so every function is thread safe.
 */
#include <string.h>
#include "lsupcrypto.h"

static const lsup_u32 K[64] = {
    0xD76AA478UL, 0xE8C7B756UL, 0x242070DBUL, 0xC1BDCEEEUL,
    0xF57C0FAFUL, 0x4787C62AUL, 0xA8304613UL, 0xFD469501UL,
    0x698098D8UL, 0x8B44F7AFUL, 0xFFFF5BB1UL, 0x895CD7BEUL,
    0x6B901122UL, 0xFD987193UL, 0xA679438EUL, 0x49B40821UL,
    0xF61E2562UL, 0xC040B340UL, 0x265E5A51UL, 0xE9B6C7AAUL,
    0xD62F105DUL, 0x02441453UL, 0xD8A1E681UL, 0xE7D3FBC8UL,
    0x21E1CDE6UL, 0xC33707D6UL, 0xF4D50D87UL, 0x455A14EDUL,
    0xA9E3E905UL, 0xFCEFA3F8UL, 0x676F02D9UL, 0x8D2A4C8AUL,
    0xFFFA3942UL, 0x8771F681UL, 0x6D9D6122UL, 0xFDE5380CUL,
    0xA4BEEA44UL, 0x4BDECFA9UL, 0xF6BB4B60UL, 0xBEBFBC70UL,
    0x289B7EC6UL, 0xEAA127FAUL, 0xD4EF3085UL, 0x04881D05UL,
    0xD9D4D039UL, 0xE6DB99E5UL, 0x1FA27CF8UL, 0xC4AC5665UL,
    0xF4292244UL, 0x432AFF97UL, 0xAB9423A7UL, 0xFC93A039UL,
    0x655B59C3UL, 0x8F0CCC92UL, 0xFFEFF47DUL, 0x85845DD1UL,
    0x6FA87E4FUL, 0xFE2CE6E0UL, 0xA3014314UL, 0x4E0811A1UL,
    0xF7537E82UL, 0xBD3AF235UL, 0x2AD7D2BBUL, 0xEB86D391UL
};

static const lsup_u32 CrcTable[256] = {
    0x00000000UL, 0x77073096UL, 0xEE0E612CUL, 0x990951BAUL, 0x076DC419UL, 0x706AF48FUL,
    0xE963A535UL, 0x9E6495A3UL, 0x0EDB8832UL, 0x79DCB8A4UL, 0xE0D5E91EUL, 0x97D2D988UL,
    0x09B64C2BUL, 0x7EB17CBDUL, 0xE7B82D07UL, 0x90BF1D91UL, 0x1DB71064UL, 0x6AB020F2UL,
    0xF3B97148UL, 0x84BE41DEUL, 0x1ADAD47DUL, 0x6DDDE4EBUL, 0xF4D4B551UL, 0x83D385C7UL,
    0x136C9856UL, 0x646BA8C0UL, 0xFD62F97AUL, 0x8A65C9ECUL, 0x14015C4FUL, 0x63066CD9UL,
    0xFA0F3D63UL, 0x8D080DF5UL, 0x3B6E20C8UL, 0x4C69105EUL, 0xD56041E4UL, 0xA2677172UL,
    0x3C03E4D1UL, 0x4B04D447UL, 0xD20D85FDUL, 0xA50AB56BUL, 0x35B5A8FAUL, 0x42B2986CUL,
    0xDBBBC9D6UL, 0xACBCF940UL, 0x32D86CE3UL, 0x45DF5C75UL, 0xDCD60DCFUL, 0xABD13D59UL,
    0x26D930ACUL, 0x51DE003AUL, 0xC8D75180UL, 0xBFD06116UL, 0x21B4F4B5UL, 0x56B3C423UL,
    0xCFBA9599UL, 0xB8BDA50FUL, 0x2802B89EUL, 0x5F058808UL, 0xC60CD9B2UL, 0xB10BE924UL,
    0x2F6F7C87UL, 0x58684C11UL, 0xC1611DABUL, 0xB6662D3DUL, 0x76DC4190UL, 0x01DB7106UL,
    0x98D220BCUL, 0xEFD5102AUL, 0x71B18589UL, 0x06B6B51FUL, 0x9FBFE4A5UL, 0xE8B8D433UL,
    0x7807C9A2UL, 0x0F00F934UL, 0x9609A88EUL, 0xE10E9818UL, 0x7F6A0DBBUL, 0x086D3D2DUL,
    0x91646C97UL, 0xE6635C01UL, 0x6B6B51F4UL, 0x1C6C6162UL, 0x856530D8UL, 0xF262004EUL,
    0x6C0695EDUL, 0x1B01A57BUL, 0x8208F4C1UL, 0xF50FC457UL, 0x65B0D9C6UL, 0x12B7E950UL,
    0x8BBEB8EAUL, 0xFCB9887CUL, 0x62DD1DDFUL, 0x15DA2D49UL, 0x8CD37CF3UL, 0xFBD44C65UL,
    0x4DB26158UL, 0x3AB551CEUL, 0xA3BC0074UL, 0xD4BB30E2UL, 0x4ADFA541UL, 0x3DD895D7UL,
    0xA4D1C46DUL, 0xD3D6F4FBUL, 0x4369E96AUL, 0x346ED9FCUL, 0xAD678846UL, 0xDA60B8D0UL,
    0x44042D73UL, 0x33031DE5UL, 0xAA0A4C5FUL, 0xDD0D7CC9UL, 0x5005713CUL, 0x270241AAUL,
    0xBE0B1010UL, 0xC90C2086UL, 0x5768B525UL, 0x206F85B3UL, 0xB966D409UL, 0xCE61E49FUL,
    0x5EDEF90EUL, 0x29D9C998UL, 0xB0D09822UL, 0xC7D7A8B4UL, 0x59B33D17UL, 0x2EB40D81UL,
    0xB7BD5C3BUL, 0xC0BA6CADUL, 0xEDB88320UL, 0x9ABFB3B6UL, 0x03B6E20CUL, 0x74B1D29AUL,
    0xEAD54739UL, 0x9DD277AFUL, 0x04DB2615UL, 0x73DC1683UL, 0xE3630B12UL, 0x94643B84UL,
    0x0D6D6A3EUL, 0x7A6A5AA8UL, 0xE40ECF0BUL, 0x9309FF9DUL, 0x0A00AE27UL, 0x7D079EB1UL,
    0xF00F9344UL, 0x8708A3D2UL, 0x1E01F268UL, 0x6906C2FEUL, 0xF762575DUL, 0x806567CBUL,
    0x196C3671UL, 0x6E6B06E7UL, 0xFED41B76UL, 0x89D32BE0UL, 0x10DA7A5AUL, 0x67DD4ACCUL,
    0xF9B9DF6FUL, 0x8EBEEFF9UL, 0x17B7BE43UL, 0x60B08ED5UL, 0xD6D6A3E8UL, 0xA1D1937EUL,
    0x38D8C2C4UL, 0x4FDFF252UL, 0xD1BB67F1UL, 0xA6BC5767UL, 0x3FB506DDUL, 0x48B2364BUL,
    0xD80D2BDAUL, 0xAF0A1B4CUL, 0x36034AF6UL, 0x41047A60UL, 0xDF60EFC3UL, 0xA867DF55UL,
    0x316E8EEFUL, 0x4669BE79UL, 0xCB61B38CUL, 0xBC66831AUL, 0x256FD2A0UL, 0x5268E236UL,
    0xCC0C7795UL, 0xBB0B4703UL, 0x220216B9UL, 0x5505262FUL, 0xC5BA3BBEUL, 0xB2BD0B28UL,
    0x2BB45A92UL, 0x5CB36A04UL, 0xC2D7FFA7UL, 0xB5D0CF31UL, 0x2CD99E8BUL, 0x5BDEAE1DUL,
    0x9B64C2B0UL, 0xEC63F226UL, 0x756AA39CUL, 0x026D930AUL, 0x9C0906A9UL, 0xEB0E363FUL,
    0x72076785UL, 0x05005713UL, 0x95BF4A82UL, 0xE2B87A14UL, 0x7BB12BAEUL, 0x0CB61B38UL,
    0x92D28E9BUL, 0xE5D5BE0DUL, 0x7CDCEFB7UL, 0x0BDBDF21UL, 0x86D3D2D4UL, 0xF1D4E242UL,
    0x68DDB3F8UL, 0x1FDA836EUL, 0x81BE16CDUL, 0xF6B9265BUL, 0x6FB077E1UL, 0x18B74777UL,
    0x88085AE6UL, 0xFF0F6A70UL, 0x66063BCAUL, 0x11010B5CUL, 0x8F659EFFUL, 0xF862AE69UL,
    0x616BFFD3UL, 0x166CCF45UL, 0xA00AE278UL, 0xD70DD2EEUL, 0x4E048354UL, 0x3903B3C2UL,
    0xA7672661UL, 0xD06016F7UL, 0x4969474DUL, 0x3E6E77DBUL, 0xAED16A4AUL, 0xD9D65ADCUL,
    0x40DF0B66UL, 0x37D83BF0UL, 0xA9BCAE53UL, 0xDEBB9EC5UL, 0x47B2CF7FUL, 0x30B5FFE9UL,
    0xBDBDF21CUL, 0xCABAC28AUL, 0x53B39330UL, 0x24B4A3A6UL, 0xBAD03605UL, 0xCDD70693UL,
    0x54DE5729UL, 0x23D967BFUL, 0xB3667A2EUL, 0xC4614AB8UL, 0x5D681B02UL, 0x2A6F2B94UL,
    0xB40BBE37UL, 0xC30C8EA1UL, 0x5A05DF1BUL, 0x2D02EF8DUL
};

static const int S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

#define ROTL(x, n) ((lsup_u32)(((x) << (n)) | (((x) & 0xFFFFFFFFUL) >> (32 - (n)))))

static void Md5Block(lsup_u32 st[4], const lsup_u8 *p)
{
    lsup_u32 m[16], a, b, c, d, f, t;
    int i, g;

    for (i = 0; i < 16; i++)
        m[i] = (lsup_u32)p[i * 4] | ((lsup_u32)p[i * 4 + 1] << 8) |
               ((lsup_u32)p[i * 4 + 2] << 16) | ((lsup_u32)p[i * 4 + 3] << 24);

    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (b & c) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);        g = (7 * i) & 15; }
        t = d;
        d = c;
        c = b;
        b = b + ROTL((a + f + K[i] + m[g]) & 0xFFFFFFFFUL, S[i]);
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void LsupMd5Init(LSUP_MD5 *ctx)
{
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xEFCDAB89UL;
    ctx->state[2] = 0x98BADCFEUL;
    ctx->state[3] = 0x10325476UL;
    ctx->bytesLo = 0;
    ctx->bytesHi = 0;
}

void LsupMd5Update(LSUP_MD5 *ctx, const void *data, unsigned long len)
{
    const lsup_u8 *p = (const lsup_u8 *)data;
    unsigned long used = ctx->bytesLo & 63, take;

    ctx->bytesLo += (lsup_u32)len;
    if (ctx->bytesLo < (lsup_u32)len)
        ctx->bytesHi++;

    if (used) {
        take = 64 - used;
        if (take > len) take = len;
        memcpy(ctx->block + used, p, take);
        p += take; len -= take;
        if (used + take < 64)
            return;
        Md5Block(ctx->state, ctx->block);
    }
    while (len >= 64) {
        Md5Block(ctx->state, p);
        p += 64; len -= 64;
    }
    if (len)
        memcpy(ctx->block, p, len);
}

void LsupMd5Final(LSUP_MD5 *ctx, lsup_u8 digest[16])
{
    lsup_u8 tail[72];
    unsigned long used = ctx->bytesLo & 63;
    unsigned long padLen = (used < 56) ? (56 - used) : (120 - used);
    lsup_u32 lo = ctx->bytesLo << 3, hi = (ctx->bytesHi << 3) | (ctx->bytesLo >> 29);
    int i;

    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    for (i = 0; i < 4; i++) {
        tail[padLen + i]     = (lsup_u8)(lo >> (8 * i));
        tail[padLen + 4 + i] = (lsup_u8)(hi >> (8 * i));
    }
    LsupMd5Update(ctx, tail, padLen + 8);
    for (i = 0; i < 16; i++)
        digest[i] = (lsup_u8)(ctx->state[i >> 2] >> (8 * (i & 3)));
    memset(ctx, 0, sizeof(*ctx));
}

void LsupMd5(const void *data, unsigned long len, lsup_u8 digest[16])
{
    LSUP_MD5 c;
    LsupMd5Init(&c);
    LsupMd5Update(&c, data, len);
    LsupMd5Final(&c, digest);
}

void LsupHmacMd5(const lsup_u8 *key, unsigned long keyLen,
                 const void *p1, unsigned long n1, const void *p2, unsigned long n2,
                 const void *p3, unsigned long n3, const void *p4, unsigned long n4,
                 lsup_u8 mac[16])
{
    lsup_u8 k[64], pad[64], inner[16];
    LSUP_MD5 c;
    int i;

    memset(k, 0, sizeof(k));
    if (keyLen > 64)
        LsupMd5(key, keyLen, k);
    else if (keyLen > 0)
        memcpy(k, key, keyLen);

    for (i = 0; i < 64; i++) pad[i] = (lsup_u8)(k[i] ^ 0x36);
    LsupMd5Init(&c);
    LsupMd5Update(&c, pad, 64);
    if (p1 && n1) LsupMd5Update(&c, p1, n1);
    if (p2 && n2) LsupMd5Update(&c, p2, n2);
    if (p3 && n3) LsupMd5Update(&c, p3, n3);
    if (p4 && n4) LsupMd5Update(&c, p4, n4);
    LsupMd5Final(&c, inner);

    for (i = 0; i < 64; i++) pad[i] = (lsup_u8)(k[i] ^ 0x5C);
    LsupMd5Init(&c);
    LsupMd5Update(&c, pad, 64);
    LsupMd5Update(&c, inner, 16);
    LsupMd5Final(&c, mac);

    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}

lsup_u32 LsupCrcUpdate(lsup_u32 crc, const void *data, unsigned long len)
{
    const lsup_u8 *p = (const lsup_u8 *)data;
    while (len--)
        crc = CrcTable[(crc ^ *p++) & 0xFF] ^ ((crc & 0xFFFFFFFFUL) >> 8);
    return crc & 0xFFFFFFFFUL;
}
