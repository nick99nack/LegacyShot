/*
 * imgenc.c - BMP, PNG and GIF encoders. Plain C89, no libraries, no CryptoAPI.
 *
 * PNG: adaptive per-row filters (minimum sum of absolute differences), then
 *      deflate with LZ77 hash chains and dynamic Huffman blocks (RFC 1951),
 *      wrapped in zlib (RFC 1950). Images with 256 colours or fewer are written
 *      as palette PNGs, packed to 1/2/4/8 bits per pixel.
 * GIF: GIF89a, one frame. Exact palette when the picture has 256 colours or
 *      fewer (typical of 8-bit desktops), otherwise median cut over a 5-5-5
 *      histogram. LZW with 12-bit codes and a clear code when the table fills.
 * BMP: 24-bit, written straight from the DIB.
 */
#include <stdlib.h>
#include <string.h>
#include "imgenc.h"
#include "lsupcrypto.h"          /* CRC-32 for PNG chunks */

/* ================================================================== buffers */

void BufFree(LS_BUF *b)
{
    if (b->data) free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
    b->failed = 0;
}

static int BufReserve(LS_BUF *b, unsigned long extra)
{
    unsigned long need, ncap;
    unsigned char *p;
    if (b->failed)
        return 0;
    need = b->size + extra;
    if (need <= b->cap)
        return 1;
    ncap = b->cap ? b->cap : 65536UL;
    while (ncap < need)
        ncap *= 2;
    p = (unsigned char *)realloc(b->data, ncap);
    if (p == NULL) {
        b->failed = 1;
        return 0;
    }
    b->data = p;
    b->cap = ncap;
    return 1;
}

static void BufPut(LS_BUF *b, const void *p, unsigned long n)
{
    if (n == 0 || !BufReserve(b, n))
        return;
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void BufByte(LS_BUF *b, unsigned int v)
{
    if (b->size < b->cap && !b->failed) {
        b->data[b->size++] = (unsigned char)v;
        return;
    }
    if (!BufReserve(b, 1))
        return;
    b->data[b->size++] = (unsigned char)v;
}

static void BufU16LE(LS_BUF *b, unsigned long v) { BufByte(b, v & 0xFF); BufByte(b, (v >> 8) & 0xFF); }
static void BufU32LE(LS_BUF *b, unsigned long v) { BufU16LE(b, v & 0xFFFF); BufU16LE(b, (v >> 16) & 0xFFFF); }
static void BufU32BE(LS_BUF *b, unsigned long v)
{
    BufByte(b, (v >> 24) & 0xFF); BufByte(b, (v >> 16) & 0xFF);
    BufByte(b, (v >> 8) & 0xFF);  BufByte(b, v & 0xFF);
}

/* ================================================================== images */

long ImgStride(int width) { return ((long)width * 3 + 3) & ~3L; }

int ImgAlloc(LS_IMAGE *img, int width, int height)
{
    img->width = width;
    img->height = height;
    img->stride = ImgStride(width);
    img->bits = NULL;
    if (width <= 0 || height <= 0)
        return 0;
    img->bits = (unsigned char *)calloc((size_t)img->stride, (size_t)height);
    return img->bits != NULL;
}

void ImgFree(LS_IMAGE *img)
{
    if (img->bits) free(img->bits);
    img->bits = NULL;
    img->width = img->height = 0;
}

/* Row of the picture counted from the top. */
#define TOPROW(img, y) ((img)->bits + (long)((img)->height - 1 - (y)) * (img)->stride)

int ImgCrop(const LS_IMAGE *src, int x, int y, int w, int h, LS_IMAGE *dst)
{
    int r;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > src->width)  w = src->width - x;
    if (y + h > src->height) h = src->height - y;
    if (w <= 0 || h <= 0 || !ImgAlloc(dst, w, h))
        return 0;
    for (r = 0; r < h; r++)
        memcpy(TOPROW(dst, r), TOPROW(src, y + r) + (long)x * 3, (size_t)w * 3);
    return 1;
}

static unsigned long PixelAt(const unsigned char *row, int x)
{
    const unsigned char *p = row + x * 3;
    return ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) | p[0];   /* 0xRRGGBB */
}

/* ------------------------------------------------------------------ exact palette */

#define CHASH 2048               /* power of two, well above 256 entries */

typedef struct {
    unsigned long key[CHASH];    /* colour + 1; 0 = empty slot */
    unsigned char idx[CHASH];
    unsigned long pal[256];      /* 0xRRGGBB */
    int count;
} COLOR_SET;

/* Returns the palette index of colour, adding it if new; -1 once more than 256 colours. */
static int ColorIndex(COLOR_SET *cs, unsigned long color)
{
    unsigned long k = color + 1, h = (color * 2654435761UL) >> 7;
    for (;;) {
        h &= CHASH - 1;
        if (cs->key[h] == k)
            return cs->idx[h];
        if (cs->key[h] == 0) {
            if (cs->count >= 256)
                return -1;
            cs->key[h] = k;
            cs->idx[h] = (unsigned char)cs->count;
            cs->pal[cs->count] = color;
            return cs->count++;
        }
        h++;
    }
}

/*
 * Collects the palette of img when it has 256 colours or fewer and, if
 * indices is not NULL, writes one index per pixel, top row first.
 * Returns the number of colours, or 0 if there are more than 256.
 */
static int ExactPalette(const LS_IMAGE *img, COLOR_SET *cs, unsigned char *indices)
{
    int x, y;
    unsigned long last = 0xFFFFFFFFUL;
    int lastIdx = 0;
    memset(cs, 0, sizeof(*cs));
    for (y = 0; y < img->height; y++) {
        const unsigned char *row = TOPROW(img, y);
        for (x = 0; x < img->width; x++) {
            unsigned long c = PixelAt(row, x);
            if (c != last) {
                lastIdx = ColorIndex(cs, c);
                if (lastIdx < 0)
                    return 0;
                last = c;
            }
            if (indices)
                *indices++ = (unsigned char)lastIdx;
        }
    }
    return cs->count;
}

int ImgCountColors(const LS_IMAGE *img)
{
    COLOR_SET *cs = (COLOR_SET *)malloc(sizeof(COLOR_SET));
    int n;
    if (cs == NULL)
        return 257;
    n = ExactPalette(img, cs, NULL);
    free(cs);
    return n ? n : 257;
}

/* ================================================================== BMP */

int ImgEncodeBmp(const LS_IMAGE *img, LS_BUF *out)
{
    unsigned long imageSize = (unsigned long)img->stride * (unsigned long)img->height;
    if (img->width <= 0 || img->height <= 0)
        return IMG_E_SIZE;
    out->size = 0;
    if (!BufReserve(out, 54 + imageSize))
        return IMG_E_NOMEM;
    /* BITMAPFILEHEADER */
    BufByte(out, 'B'); BufByte(out, 'M');
    BufU32LE(out, 54 + imageSize);
    BufU32LE(out, 0);
    BufU32LE(out, 54);
    /* BITMAPINFOHEADER: positive height = bottom-up, which is how we store it */
    BufU32LE(out, 40);
    BufU32LE(out, (unsigned long)img->width);
    BufU32LE(out, (unsigned long)img->height);
    BufU16LE(out, 1);
    BufU16LE(out, 24);
    BufU32LE(out, 0);            /* BI_RGB */
    BufU32LE(out, imageSize);
    BufU32LE(out, 2835);         /* 72 dpi */
    BufU32LE(out, 2835);
    BufU32LE(out, 0);
    BufU32LE(out, 0);
    BufPut(out, img->bits, imageSize);
    return out->failed ? IMG_E_NOMEM : IMG_OK;
}

/* ================================================================== deflate */

#define WSIZE      32768L
#define WMASK      (WSIZE - 1)
#define HBITS      15
#define HSIZE      (1L << HBITS)
#define MAX_CHAIN  96
#define NICE_LEN   128
#define MIN_MATCH  3
#define MAX_MATCH  258
#define BLOCK_SYMS 32768L

static const unsigned short LenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char LenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short DistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char DistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const unsigned char ClOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

typedef struct {
    LS_BUF *out;
    unsigned long bitBuf;
    int bitCount;

    long *head;                  /* HSIZE entries: most recent position per hash, -1 = none */
    long *prev;                  /* WSIZE entries: previous position with the same hash */
    unsigned short *symLit;      /* literal byte, or 256 + match length */
    unsigned short *symDist;     /* 0 for a literal */
    long nsyms;

    unsigned char lenCode[MAX_MATCH + 1];   /* match length -> index into LenBase */
} DEFLATE;

static void PutBits(DEFLATE *d, unsigned long value, int n)
{
    d->bitBuf |= (value & ((1UL << n) - 1)) << d->bitCount;
    d->bitCount += n;
    while (d->bitCount >= 8) {
        BufByte(d->out, (unsigned int)(d->bitBuf & 0xFF));
        d->bitBuf >>= 8;
        d->bitCount -= 8;
    }
}

static void AlignByte(DEFLATE *d)
{
    if (d->bitCount > 0)
        PutBits(d, 0, 8 - d->bitCount);
}

static int DistCode(unsigned int dist)
{
    int i = 29;
    while (DistBase[i] > dist)
        i--;
    return i;
}

/*
 * Huffman code lengths for freq[0..n-1], limited to maxBits. If the tree is
 * too deep the frequencies are halved (keeping every used symbol at >= 1)
 * and the tree rebuilt; this always terminates and keeps the code complete.
 */
static void BuildLengths(const unsigned long *freqIn, int n, int maxBits, unsigned char *lens)
{
    unsigned long f[286], w[2 * 286];
    int parent[2 * 286], alive[2 * 286];
    int i, nodes, used, maxDepth;

    for (i = 0; i < n; i++)
        f[i] = freqIn[i];

    for (;;) {
        used = 0;
        for (i = 0; i < n; i++) {
            lens[i] = 0;
            w[i] = f[i];
            parent[i] = -1;
            alive[i] = f[i] > 0;
            if (f[i] > 0) used++;
        }
        if (used == 0)
            return;
        if (used == 1) {
            for (i = 0; i < n; i++)
                if (f[i] > 0) lens[i] = 1;
            return;
        }
        nodes = n;
        for (;;) {
            int a = -1, b = -1;
            for (i = 0; i < nodes; i++) {
                if (!alive[i]) continue;
                if (a < 0 || w[i] < w[a]) { b = a; a = i; }
                else if (b < 0 || w[i] < w[b]) { b = i; }
            }
            if (b < 0)
                break;           /* a is the root */
            w[nodes] = w[a] + w[b];
            parent[nodes] = -1;
            alive[nodes] = 1;
            parent[a] = parent[b] = nodes;
            alive[a] = alive[b] = 0;
            nodes++;
        }
        maxDepth = 0;
        for (i = 0; i < n; i++) {
            int dpt = 0, j = i;
            if (f[i] == 0) continue;
            while (parent[j] >= 0) { j = parent[j]; dpt++; }
            lens[i] = (unsigned char)dpt;
            if (dpt > maxDepth) maxDepth = dpt;
        }
        if (maxDepth <= maxBits)
            return;
        for (i = 0; i < n; i++)
            if (f[i] > 0) f[i] = (f[i] + 1) / 2;
    }
}

/* Canonical codes (RFC 1951 3.2.2), bit-reversed for LSB-first output. */
static void BuildCodes(const unsigned char *lens, int n, unsigned short *codes)
{
    unsigned short blCount[16], nextCode[16];
    int i, bits;
    unsigned short code = 0;
    memset(blCount, 0, sizeof(blCount));
    for (i = 0; i < n; i++) blCount[lens[i]]++;
    blCount[0] = 0;
    for (bits = 1; bits < 16; bits++) {
        code = (unsigned short)((code + blCount[bits - 1]) << 1);
        nextCode[bits] = code;
    }
    for (i = 0; i < n; i++) {
        int len = lens[i];
        codes[i] = 0;
        if (len) {
            unsigned int c = nextCode[len]++, r = 0;
            int k;
            for (k = 0; k < len; k++) { r = (r << 1) | (c & 1); c >>= 1; }
            codes[i] = (unsigned short)r;
        }
    }
}

/* Writes the block made of the pending symbols, covering in[rawStart..rawEnd). */
static void FlushBlock(DEFLATE *d, const unsigned char *in, long rawStart, long rawEnd, int last)
{
    unsigned long litF[286], distF[30], clF[19];
    unsigned char litL[286], distL[30], clL[19], all[286 + 30];
    unsigned short litC[286], distC[30], clC[19];
    unsigned char rle[286 + 30], rleExtra[286 + 30];
    int nrle = 0, hlit, hdist, hclen, i, nonzero;
    unsigned long dynBits, storedBits;
    long s;

    memset(litF, 0, sizeof(litF));
    memset(distF, 0, sizeof(distF));
    for (s = 0; s < d->nsyms; s++) {
        unsigned int v = d->symLit[s];
        if (d->symDist[s] == 0) {
            litF[v]++;
        } else {
            litF[257 + d->lenCode[v - 256]]++;
            distF[DistCode(d->symDist[s])]++;
        }
    }
    litF[256] = 1;                                   /* end of block */
    nonzero = 0;
    for (i = 0; i < 286; i++) if (litF[i]) nonzero++;
    if (nonzero < 2) litF[0]++;                      /* keep the tree complete */
    nonzero = 0;
    for (i = 0; i < 30; i++) if (distF[i]) nonzero++;
    if (nonzero == 0) { distF[0] = 1; distF[1] = 1; }
    else if (nonzero == 1) { if (distF[0]) distF[1] = 1; else distF[0] = 1; }

    BuildLengths(litF, 286, 15, litL);
    BuildLengths(distF, 30, 15, distL);

    for (hlit = 286; hlit > 257 && litL[hlit - 1] == 0; hlit--) ;
    for (hdist = 30; hdist > 1 && distL[hdist - 1] == 0; hdist--) ;
    memcpy(all, litL, hlit);
    memcpy(all + hlit, distL, hdist);

    /* Run-length encode the code lengths with symbols 16, 17 and 18. */
    memset(clF, 0, sizeof(clF));
    for (i = 0; i < hlit + hdist; ) {
        int v = all[i], run = 1;
        while (i + run < hlit + hdist && all[i + run] == v) run++;
        if (v == 0 && run >= 3) {
            int take = run > 138 ? 138 : run;
            if (take >= 11) { rle[nrle] = 18; rleExtra[nrle++] = (unsigned char)(take - 11); }
            else            { rle[nrle] = 17; rleExtra[nrle++] = (unsigned char)(take - 3); }
            clF[rle[nrle - 1]]++;
            i += take;
        } else if (v != 0 && run >= 4) {
            int take = run - 1 > 6 ? 6 : run - 1;
            rle[nrle] = (unsigned char)v; rleExtra[nrle++] = 0; clF[v]++;
            rle[nrle] = 16; rleExtra[nrle++] = (unsigned char)(take - 3); clF[16]++;
            i += 1 + take;
        } else {
            rle[nrle] = (unsigned char)v; rleExtra[nrle++] = 0; clF[v]++;
            i++;
        }
    }
    nonzero = 0;
    for (i = 0; i < 19; i++) if (clF[i]) nonzero++;
    if (nonzero < 2) { if (clF[0]) clF[1]++; else clF[0]++; }
    BuildLengths(clF, 19, 7, clL);
    for (hclen = 19; hclen > 4 && clL[ClOrder[hclen - 1]] == 0; hclen--) ;

    BuildCodes(litL, 286, litC);
    BuildCodes(distL, 30, distC);
    BuildCodes(clL, 19, clC);

    /* Cost of the dynamic block versus storing the bytes. */
    dynBits = 3 + 14 + 3UL * hclen;
    for (i = 0; i < nrle; i++)
        dynBits += clL[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
    for (s = 0; s < d->nsyms; s++) {
        if (d->symDist[s] == 0) {
            dynBits += litL[d->symLit[s]];
        } else {
            int lc = d->lenCode[d->symLit[s] - 256], dc = DistCode(d->symDist[s]);
            dynBits += litL[257 + lc] + LenExtra[lc] + distL[dc] + DistExtra[dc];
        }
    }
    dynBits += litL[256];
    storedBits = (unsigned long)(rawEnd - rawStart) * 8 + ((unsigned long)(rawEnd - rawStart) / 65535 + 1) * 40 + 10;

    if (storedBits < dynBits) {
        long pos = rawStart;
        do {
            long n = rawEnd - pos;
            if (n > 65535) n = 65535;
            PutBits(d, (last && pos + n >= rawEnd) ? 1 : 0, 1);
            PutBits(d, 0, 2);
            AlignByte(d);
            BufU16LE(d->out, (unsigned long)n);
            BufU16LE(d->out, (unsigned long)(~n & 0xFFFF));
            BufPut(d->out, in + pos, (unsigned long)n);
            pos += n;
        } while (pos < rawEnd);
    } else {
        PutBits(d, last ? 1 : 0, 1);
        PutBits(d, 2, 2);                            /* BTYPE 10: dynamic Huffman */
        PutBits(d, (unsigned long)(hlit - 257), 5);
        PutBits(d, (unsigned long)(hdist - 1), 5);
        PutBits(d, (unsigned long)(hclen - 4), 4);
        for (i = 0; i < hclen; i++)
            PutBits(d, clL[ClOrder[i]], 3);
        for (i = 0; i < nrle; i++) {
            PutBits(d, clC[rle[i]], clL[rle[i]]);
            if (rle[i] == 16) PutBits(d, rleExtra[i], 2);
            else if (rle[i] == 17) PutBits(d, rleExtra[i], 3);
            else if (rle[i] == 18) PutBits(d, rleExtra[i], 7);
        }
        for (s = 0; s < d->nsyms; s++) {
            unsigned int v = d->symLit[s];
            if (d->symDist[s] == 0) {
                PutBits(d, litC[v], litL[v]);
            } else {
                unsigned int len = v - 256, dist = d->symDist[s];
                int lc = d->lenCode[len], dc = DistCode(dist);
                PutBits(d, litC[257 + lc], litL[257 + lc]);
                if (LenExtra[lc]) PutBits(d, len - LenBase[lc], LenExtra[lc]);
                PutBits(d, distC[dc], distL[dc]);
                if (DistExtra[dc]) PutBits(d, dist - DistBase[dc], DistExtra[dc]);
            }
        }
        PutBits(d, litC[256], litL[256]);
    }
    d->nsyms = 0;
}

#define HASH3(p) ((((unsigned long)(p)[0] << 10) ^ ((unsigned long)(p)[1] << 5) ^ (p)[2]) & (HSIZE - 1))

/* zlib stream (header, deflate data, Adler-32) of in[0..n) appended to out. */
static int Zlib(const unsigned char *in, long n, LS_BUF *out,
                IMG_PROGRESS_FN progress, void *ctx, unsigned long pdone, unsigned long ptotal,
                volatile long *cancel)
{
    DEFLATE d;
    long pos = 0, blockStart = 0, i, nextReport = 65536L;
    unsigned long s1 = 1, s2 = 0;
    int rc = IMG_OK;

    memset(&d, 0, sizeof(d));
    d.out = out;
    d.head = (long *)malloc(sizeof(long) * HSIZE);
    d.prev = (long *)malloc(sizeof(long) * WSIZE);
    d.symLit = (unsigned short *)malloc(sizeof(unsigned short) * BLOCK_SYMS);
    d.symDist = (unsigned short *)malloc(sizeof(unsigned short) * BLOCK_SYMS);
    if (!d.head || !d.prev || !d.symLit || !d.symDist) {
        rc = IMG_E_NOMEM;
        goto done;
    }
    for (i = 0; i < HSIZE; i++) d.head[i] = -1;
    for (i = 0; i < WSIZE; i++) d.prev[i] = -1;
    for (i = 0; i < 29; i++) {
        int l, top = (i == 28) ? 258 : (i == 27 ? 257 : LenBase[i + 1] - 1);
        for (l = LenBase[i]; l <= top; l++) d.lenCode[l] = (unsigned char)i;
    }

    BufByte(out, 0x78);
    BufByte(out, 0x9C);

    while (pos < n) {
        long bestLen = 0, bestDist = 0;
        if (pos + MIN_MATCH <= n) {
            unsigned long h = HASH3(in + pos);
            long cand = d.head[h], maxLen = n - pos;
            int chain = MAX_CHAIN;
            if (maxLen > MAX_MATCH) maxLen = MAX_MATCH;
            while (cand >= 0 && pos - cand <= WSIZE && chain-- > 0) {
                long nc;
                if (in[cand + bestLen] == in[pos + bestLen] && in[cand] == in[pos]) {
                    long len = 0;
                    while (len < maxLen && in[cand + len] == in[pos + len]) len++;
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = pos - cand;
                        if (len >= NICE_LEN || len == maxLen) break;
                    }
                }
                nc = d.prev[cand & WMASK];
                if (nc >= cand) break;   /* slot reused by a newer position */
                cand = nc;
            }
            d.prev[pos & WMASK] = d.head[h];
            d.head[h] = pos;
        }

        if (bestLen >= MIN_MATCH) {
            d.symLit[d.nsyms] = (unsigned short)(256 + bestLen);
            d.symDist[d.nsyms] = (unsigned short)bestDist;
            d.nsyms++;
            for (i = 1; i < bestLen; i++) {
                long q = pos + i;
                if (q + MIN_MATCH <= n) {
                    unsigned long h = HASH3(in + q);
                    d.prev[q & WMASK] = d.head[h];
                    d.head[h] = q;
                }
            }
            pos += bestLen;
        } else {
            d.symLit[d.nsyms] = in[pos];
            d.symDist[d.nsyms] = 0;
            d.nsyms++;
            pos++;
        }

        if (d.nsyms >= BLOCK_SYMS) {
            FlushBlock(&d, in, blockStart, pos, 0);
            blockStart = pos;
        }
        if (pos >= nextReport) {
            nextReport = pos + 65536L;
            if (cancel && *cancel) { rc = IMG_E_CANCEL; goto done; }
            if (out->failed) { rc = IMG_E_NOMEM; goto done; }
            if (progress)
                progress(ctx, pdone + (unsigned long)((double)ptotal * pos / n), pdone + ptotal);
        }
    }
    FlushBlock(&d, in, blockStart, pos, 1);
    AlignByte(&d);

    for (pos = 0; pos < n; ) {
        long k = n - pos;
        if (k > 5552) k = 5552;
        for (i = 0; i < k; i++) { s1 += in[pos + i]; s2 += s1; }
        s1 %= 65521UL;
        s2 %= 65521UL;
        pos += k;
    }
    BufU32BE(out, (s2 << 16) | s1);
    if (out->failed)
        rc = IMG_E_NOMEM;

done:
    if (d.head) free(d.head);
    if (d.prev) free(d.prev);
    if (d.symLit) free(d.symLit);
    if (d.symDist) free(d.symDist);
    return rc;
}

/* ================================================================== PNG */

static void PngChunk(LS_BUF *out, const char *type, const unsigned char *data, unsigned long len)
{
    lsup_u32 crc;
    BufU32BE(out, len);
    BufPut(out, type, 4);
    BufPut(out, data, len);
    crc = LsupCrcUpdate(LSUP_CRC_INIT, type, 4);
    crc = LsupCrcFinish(LsupCrcUpdate(crc, data, len));
    BufU32BE(out, crc);
}

static int Paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p - a, pb = p - b, pc = p - c;
    if (pa < 0) pa = -pa;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int ImgEncodePng(const LS_IMAGE *img, LS_BUF *out,
                 IMG_PROGRESS_FN progress, void *ctx, volatile long *cancel)
{
    COLOR_SET *cs = NULL;
    unsigned char *raw = NULL, *cur = NULL, *prv = NULL, *cand = NULL, *idx = NULL;
    LS_BUF z;
    unsigned char ihdr[13], plte[768];
    long rowBytes, rawSize, y, x;
    int colors = 0, depth = 8, rc = IMG_OK, w = img->width, h = img->height;
    unsigned long total = (unsigned long)h * 2;

    memset(&z, 0, sizeof(z));
    out->size = 0;
    if (w <= 0 || h <= 0)
        return IMG_E_SIZE;

    cs = (COLOR_SET *)malloc(sizeof(COLOR_SET));
    idx = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (cs != NULL && idx != NULL)
        colors = ExactPalette(img, cs, idx);
    if (colors == 0 && idx != NULL) { free(idx); idx = NULL; }

    if (colors > 0) {
        depth = colors <= 2 ? 1 : colors <= 4 ? 2 : colors <= 16 ? 4 : 8;
        rowBytes = ((long)w * depth + 7) / 8;
    } else {
        rowBytes = (long)w * 3;
    }
    rawSize = (rowBytes + 1) * h;
    raw = (unsigned char *)malloc((size_t)rawSize);
    cur = (unsigned char *)malloc((size_t)rowBytes);
    prv = (unsigned char *)calloc(1, (size_t)rowBytes);
    cand = (unsigned char *)malloc((size_t)rowBytes * 4);
    if (!raw || !cur || !prv || !cand) {
        rc = IMG_E_NOMEM;
        goto done;
    }

    for (y = 0; y < h; y++) {
        unsigned char *dst = raw + y * (rowBytes + 1);
        if (colors > 0) {
            /* Palette rows: pack indices MSB first, filter type 0 (recommended for palettes). */
            const unsigned char *ip = idx + y * (long)w;
            dst[0] = 0;
            memset(dst + 1, 0, (size_t)rowBytes);
            for (x = 0; x < w; x++) {
                long bit = x * depth;
                dst[1 + bit / 8] |= (unsigned char)(ip[x] << (8 - depth - (bit % 8)));
            }
        } else {
            const unsigned char *src = TOPROW(img, y);
            unsigned long best = 0xFFFFFFFFUL;
            int f, bestF = 0;
            for (x = 0; x < w; x++) {             /* BGR -> RGB */
                cur[x * 3]     = src[x * 3 + 2];
                cur[x * 3 + 1] = src[x * 3 + 1];
                cur[x * 3 + 2] = src[x * 3];
            }
            /* Try Sub, Up, Average, Paeth (and None); keep the one with the smallest sum. */
            for (f = 0; f < 5; f++) {
                unsigned char *o = (f == 0) ? dst + 1 : cand + (long)(f - 1) * rowBytes;
                unsigned long sum = 0;
                for (x = 0; x < rowBytes; x++) {
                    int a = x >= 3 ? cur[x - 3] : 0, b = prv[x], c = x >= 3 ? prv[x - 3] : 0, v;
                    switch (f) {
                    case 0:  v = cur[x]; break;
                    case 1:  v = cur[x] - a; break;
                    case 2:  v = cur[x] - b; break;
                    case 3:  v = cur[x] - ((a + b) >> 1); break;
                    default: v = cur[x] - Paeth(a, b, c); break;
                    }
                    o[x] = (unsigned char)v;
                    sum += (o[x] < 128) ? o[x] : 256 - o[x];
                }
                if (sum < best) { best = sum; bestF = f; }
            }
            dst[0] = (unsigned char)bestF;
            if (bestF != 0)
                memcpy(dst + 1, cand + (long)(bestF - 1) * rowBytes, (size_t)rowBytes);
            memcpy(prv, cur, (size_t)rowBytes);
        }
        if ((y & 31) == 0) {
            if (cancel && *cancel) { rc = IMG_E_CANCEL; goto done; }
            if (progress) progress(ctx, (unsigned long)y, total);
        }
    }

    rc = Zlib(raw, rawSize, &z, progress, ctx, (unsigned long)h, (unsigned long)h, cancel);
    if (rc != IMG_OK)
        goto done;

    BufPut(out, "\x89PNG\r\n\x1A\n", 8);
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);  ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);  ihdr[7] = (unsigned char)h;
    ihdr[8] = (unsigned char)depth;
    ihdr[9] = (unsigned char)(colors > 0 ? 3 : 2);   /* palette or truecolour */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    PngChunk(out, "IHDR", ihdr, 13);
    if (colors > 0) {
        int i;
        for (i = 0; i < colors; i++) {
            plte[i * 3]     = (unsigned char)(cs->pal[i] >> 16);
            plte[i * 3 + 1] = (unsigned char)(cs->pal[i] >> 8);
            plte[i * 3 + 2] = (unsigned char)cs->pal[i];
        }
        PngChunk(out, "PLTE", plte, (unsigned long)colors * 3);
    }
    {
        unsigned long off = 0;
        while (off < z.size) {
            unsigned long n = z.size - off;
            if (n > 65536UL) n = 65536UL;
            PngChunk(out, "IDAT", z.data + off, n);
            off += n;
        }
    }
    PngChunk(out, "IEND", NULL, 0);
    if (out->failed)
        rc = IMG_E_NOMEM;
    if (progress) progress(ctx, total, total);

done:
    BufFree(&z);
    if (cs) free(cs);
    if (idx) free(idx);
    if (raw) free(raw);
    if (cur) free(cur);
    if (prv) free(prv);
    if (cand) free(cand);
    return rc;
}

/* ================================================================== GIF */

typedef struct { int lo[3], hi[3]; unsigned long count; } BOX;   /* inclusive 5-bit ranges, r g b */

#define BIN(r, g, b) (((long)(r) << 10) | ((long)(g) << 5) | (long)(b))

static void ShrinkBox(BOX *bx, const unsigned long *hist)
{
    int lo[3], hi[3], r, g, b;
    unsigned long cnt = 0;
    lo[0] = lo[1] = lo[2] = 32;
    hi[0] = hi[1] = hi[2] = -1;
    for (r = bx->lo[0]; r <= bx->hi[0]; r++)
        for (g = bx->lo[1]; g <= bx->hi[1]; g++)
            for (b = bx->lo[2]; b <= bx->hi[2]; b++) {
                unsigned long c = hist[BIN(r, g, b)];
                if (!c) continue;
                cnt += c;
                if (r < lo[0]) lo[0] = r;
                if (r > hi[0]) hi[0] = r;
                if (g < lo[1]) lo[1] = g;
                if (g > hi[1]) hi[1] = g;
                if (b < lo[2]) lo[2] = b;
                if (b > hi[2]) hi[2] = b;
            }
    if (cnt) {
        memcpy(bx->lo, lo, sizeof(lo));
        memcpy(bx->hi, hi, sizeof(hi));
    }
    bx->count = cnt;
}

/* Count of pixels in bx whose `axis` coordinate equals v. */
static unsigned long SliceCount(const BOX *bx, const unsigned long *hist, int axis, int v)
{
    int r, g, b, lo[3], hi[3];
    unsigned long cnt = 0;
    memcpy(lo, bx->lo, sizeof(lo));
    memcpy(hi, bx->hi, sizeof(hi));
    lo[axis] = hi[axis] = v;
    for (r = lo[0]; r <= hi[0]; r++)
        for (g = lo[1]; g <= hi[1]; g++)
            for (b = lo[2]; b <= hi[2]; b++)
                cnt += hist[BIN(r, g, b)];
    return cnt;
}

/*
 * Median cut: builds up to 256 colours in pal and maps every pixel to an
 * index (top row first). Returns the palette size, or 0 if out of memory.
 */
static int MedianCut(const LS_IMAGE *img, unsigned long *pal, unsigned char *indices,
                     volatile long *cancel)
{
    unsigned long *hist = (unsigned long *)calloc(32768, sizeof(unsigned long));
    unsigned long *sum = (unsigned long *)calloc(32768 * 3, sizeof(unsigned long));
    short *map = (short *)malloc(32768 * sizeof(short));
    BOX *boxes = (BOX *)malloc(256 * sizeof(BOX));
    int nb = 1, i, x, y, ncolors = 0;

    if (!hist || !sum || !map || !boxes)
        goto done;

    for (y = 0; y < img->height; y++) {
        const unsigned char *row = TOPROW(img, y);
        for (x = 0; x < img->width; x++) {
            const unsigned char *p = row + x * 3;
            long bin = BIN(p[2] >> 3, p[1] >> 3, p[0] >> 3);
            hist[bin]++;
            sum[bin * 3]     += p[2];
            sum[bin * 3 + 1] += p[1];
            sum[bin * 3 + 2] += p[0];
        }
    }

    boxes[0].lo[0] = boxes[0].lo[1] = boxes[0].lo[2] = 0;
    boxes[0].hi[0] = boxes[0].hi[1] = boxes[0].hi[2] = 31;
    ShrinkBox(&boxes[0], hist);

    while (nb < 256) {
        int best = -1, axis = 0, a, cut;
        unsigned long half, acc;
        for (i = 0; i < nb; i++) {
            int span = 0;
            for (a = 0; a < 3; a++)
                if (boxes[i].hi[a] - boxes[i].lo[a] > span) span = boxes[i].hi[a] - boxes[i].lo[a];
            if (span == 0) continue;               /* a single bin cannot be split */
            if (best < 0 || boxes[i].count > boxes[best].count) best = i;
        }
        if (best < 0)
            break;
        if (cancel && *cancel)
            break;
        /* Longest side, with green weighted a little: the eye is most sensitive to it. */
        {
            int lr = (boxes[best].hi[0] - boxes[best].lo[0]) * 3;
            int lg = (boxes[best].hi[1] - boxes[best].lo[1]) * 4;
            int lb = (boxes[best].hi[2] - boxes[best].lo[2]) * 2;
            axis = (lg >= lr && lg >= lb) ? 1 : (lr >= lb ? 0 : 2);
            if (boxes[best].hi[axis] == boxes[best].lo[axis])
                for (a = 0; a < 3; a++)
                    if (boxes[best].hi[a] > boxes[best].lo[a]) { axis = a; break; }
        }
        half = boxes[best].count / 2;
        acc = 0;
        for (cut = boxes[best].lo[axis]; cut < boxes[best].hi[axis]; cut++) {
            acc += SliceCount(&boxes[best], hist, axis, cut);
            if (acc >= half) break;
        }
        if (cut >= boxes[best].hi[axis]) cut = boxes[best].hi[axis] - 1;
        boxes[nb] = boxes[best];
        boxes[best].hi[axis] = cut;
        boxes[nb].lo[axis] = cut + 1;
        ShrinkBox(&boxes[best], hist);
        ShrinkBox(&boxes[nb], hist);
        nb++;
    }

    for (i = 0; i < nb; i++) {
        unsigned long sr = 0, sg = 0, sb = 0, cnt = 0;
        int r, g, b;
        for (r = boxes[i].lo[0]; r <= boxes[i].hi[0]; r++)
            for (g = boxes[i].lo[1]; g <= boxes[i].hi[1]; g++)
                for (b = boxes[i].lo[2]; b <= boxes[i].hi[2]; b++) {
                    long bin = BIN(r, g, b);
                    if (!hist[bin]) continue;
                    cnt += hist[bin];
                    sr += sum[bin * 3]; sg += sum[bin * 3 + 1]; sb += sum[bin * 3 + 2];
                }
        if (cnt == 0) cnt = 1;
        pal[i] = ((sr / cnt) << 16) | ((sg / cnt) << 8) | (sb / cnt);
    }

    /* Map each used bin to its nearest palette colour (computed once per bin). */
    for (i = 0; i < 32768; i++) map[i] = -1;
    for (y = 0; y < img->height; y++) {
        const unsigned char *row = TOPROW(img, y);
        for (x = 0; x < img->width; x++) {
            const unsigned char *p = row + x * 3;
            long bin = BIN(p[2] >> 3, p[1] >> 3, p[0] >> 3);
            if (map[bin] < 0) {
                long cr = (long)(sum[bin * 3] / hist[bin]), cg = (long)(sum[bin * 3 + 1] / hist[bin]),
                     cb = (long)(sum[bin * 3 + 2] / hist[bin]), bestD = 0x7FFFFFFFL;
                int k, bestK = 0;
                for (k = 0; k < nb; k++) {
                    long dr = cr - (long)(pal[k] >> 16), dg = cg - (long)((pal[k] >> 8) & 0xFF),
                         db = cb - (long)(pal[k] & 0xFF);
                    long dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                    if (dd < bestD) { bestD = dd; bestK = k; }
                }
                map[bin] = (short)bestK;
            }
            *indices++ = (unsigned char)map[bin];
        }
    }
    ncolors = nb;

done:
    if (hist) free(hist);
    if (sum) free(sum);
    if (map) free(map);
    if (boxes) free(boxes);
    return ncolors;
}

/* ------------------------------------------------------------------ LZW */

#define LZW_HSIZE 5003           /* prime, as in the classic compress/giflib encoders */

typedef struct {
    LS_BUF *out;
    unsigned long acc;
    int accBits;
    unsigned char block[256];    /* block[0] = sub-block length */
} GIFBITS;

static void GifFlushBlock(GIFBITS *g)
{
    if (g->block[0]) {
        BufPut(g->out, g->block, (unsigned long)g->block[0] + 1);
        g->block[0] = 0;
    }
}

static void GifCode(GIFBITS *g, unsigned int code, int bits)
{
    g->acc |= (unsigned long)code << g->accBits;
    g->accBits += bits;
    while (g->accBits >= 8) {
        g->block[++g->block[0]] = (unsigned char)(g->acc & 0xFF);
        if (g->block[0] == 255) GifFlushBlock(g);
        g->acc >>= 8;
        g->accBits -= 8;
    }
}

static int GifLzw(LS_BUF *out, const unsigned char *idx, unsigned long n, int minCode,
                  IMG_PROGRESS_FN progress, void *ctx, unsigned long pdone, unsigned long ptotal,
                  volatile long *cancel)
{
    long *hkey = (long *)malloc(sizeof(long) * LZW_HSIZE);
    unsigned short *hcode = (unsigned short *)malloc(sizeof(unsigned short) * LZW_HSIZE);
    GIFBITS g;
    unsigned int clear = 1u << minCode, cur, maxCode;
    int codeSize = minCode + 1;
    unsigned long i;

    if (!hkey || !hcode) {
        if (hkey) free(hkey);
        if (hcode) free(hcode);
        return IMG_E_NOMEM;
    }
    memset(&g, 0, sizeof(g));
    g.out = out;
    for (i = 0; i < LZW_HSIZE; i++) hkey[i] = -1;

    BufByte(out, (unsigned int)minCode);
    GifCode(&g, clear, codeSize);
    maxCode = clear + 1;                    /* last code in use (EOI) */
    cur = idx[0];

    for (i = 1; i < n; i++) {
        unsigned int c = idx[i];
        long key = ((long)cur << 8) | (long)c;
        long h = (((long)c << 4) ^ (long)cur) % LZW_HSIZE;
        int found = 0;
        while (hkey[h] >= 0) {
            if (hkey[h] == key) { found = 1; break; }
            if (++h >= LZW_HSIZE) h = 0;
        }
        if (found) {
            cur = hcode[h];
            continue;
        }
        GifCode(&g, cur, codeSize);
        maxCode++;
        if (maxCode < 4096) {
            hkey[h] = key;
            hcode[h] = (unsigned short)maxCode;
            if (maxCode >= (1u << codeSize) && codeSize < 12)
                codeSize++;
        }
        if (maxCode >= 4095) {              /* table full: start over */
            long k;
            GifCode(&g, clear, codeSize);
            for (k = 0; k < LZW_HSIZE; k++) hkey[k] = -1;
            codeSize = minCode + 1;
            maxCode = clear + 1;
        }
        cur = c;
        if ((i & 0xFFFF) == 0) {
            if (cancel && *cancel) { free(hkey); free(hcode); return IMG_E_CANCEL; }
            if (progress) progress(ctx, pdone + (unsigned long)((double)ptotal * i / n), pdone + ptotal);
        }
    }
    GifCode(&g, cur, codeSize);
    GifCode(&g, clear + 1, codeSize);       /* end of information */
    if (g.accBits > 0)
        GifCode(&g, 0, 8 - g.accBits);
    GifFlushBlock(&g);
    BufByte(out, 0);                        /* block terminator */
    free(hkey);
    free(hcode);
    return out->failed ? IMG_E_NOMEM : IMG_OK;
}

int ImgEncodeGif(const LS_IMAGE *img, LS_BUF *out,
                 IMG_PROGRESS_FN progress, void *ctx, volatile long *cancel)
{
    COLOR_SET *cs;
    unsigned char *idx;
    unsigned long pal[256];
    int colors, bits, i, rc;
    unsigned long n = (unsigned long)img->width * (unsigned long)img->height;

    out->size = 0;
    if (img->width <= 0 || img->height <= 0 || img->width > 65535 || img->height > 65535)
        return IMG_E_SIZE;
    cs = (COLOR_SET *)malloc(sizeof(COLOR_SET));
    idx = (unsigned char *)malloc((size_t)n);
    if (!cs || !idx) {
        if (cs) free(cs);
        if (idx) free(idx);
        return IMG_E_NOMEM;
    }
    if (progress) progress(ctx, 0, 100);
    colors = ExactPalette(img, cs, idx);
    if (colors > 0) {
        memcpy(pal, cs->pal, sizeof(unsigned long) * colors);
    } else {
        colors = MedianCut(img, pal, idx, cancel);
        if (cancel && *cancel) { free(cs); free(idx); return IMG_E_CANCEL; }
        if (colors == 0) { free(cs); free(idx); return IMG_E_NOMEM; }
    }
    free(cs);
    if (progress) progress(ctx, 20, 100);

    for (bits = 1; (1 << bits) < colors; bits++) ;

    BufPut(out, "GIF89a", 6);
    BufU16LE(out, (unsigned long)img->width);
    BufU16LE(out, (unsigned long)img->height);
    BufByte(out, 0x80 | (7 << 4) | (unsigned int)(bits - 1));   /* global table, 8-bit source */
    BufByte(out, 0);                                            /* background index */
    BufByte(out, 0);                                            /* aspect ratio */
    for (i = 0; i < (1 << bits); i++) {
        unsigned long c = i < colors ? pal[i] : 0;
        BufByte(out, (unsigned int)(c >> 16) & 0xFF);
        BufByte(out, (unsigned int)(c >> 8) & 0xFF);
        BufByte(out, (unsigned int)c & 0xFF);
    }
    BufByte(out, 0x2C);                                         /* image descriptor */
    BufU16LE(out, 0);
    BufU16LE(out, 0);
    BufU16LE(out, (unsigned long)img->width);
    BufU16LE(out, (unsigned long)img->height);
    BufByte(out, 0);
    rc = GifLzw(out, idx, n, bits < 2 ? 2 : bits, progress, ctx, 20, 80, cancel);
    free(idx);
    if (rc != IMG_OK)
        return rc;
    BufByte(out, 0x3B);                                         /* trailer */
    if (progress) progress(ctx, 100, 100);
    return out->failed ? IMG_E_NOMEM : IMG_OK;
}

/* ================================================================== dispatch */

int ImgEncode(int format, const LS_IMAGE *img, LS_BUF *out,
              IMG_PROGRESS_FN progress, void *ctx, volatile long *cancel)
{
    switch (format) {
    case IMG_FMT_GIF: return ImgEncodeGif(img, out, progress, ctx, cancel);
    case IMG_FMT_BMP: return ImgEncodeBmp(img, out);
    default:          return ImgEncodePng(img, out, progress, ctx, cancel);
    }
}
