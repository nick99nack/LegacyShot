/*
 * imgenc.h - BMP, PNG and GIF encoders for LegacyShot. Plain C89, no libraries.
 *
 * Input is a 24-bit DIB as GetDIBits/CreateDIBSection produce it:
 * bottom-up rows, blue-green-red byte order, each row padded to 4 bytes.
 */
#ifndef IMGENC_H
#define IMGENC_H

typedef struct {
    int width;
    int height;
    long stride;                 /* bytes per row, ((width * 3 + 3) & ~3) */
    unsigned char *bits;         /* row 0 is the BOTTOM row of the picture */
} LS_IMAGE;

/* Growable output buffer. Start zeroed; free data with free(). */
typedef struct {
    unsigned char *data;
    unsigned long size;
    unsigned long cap;
    int failed;                  /* set when an allocation failed */
} LS_BUF;

/* Progress: done/total in arbitrary units (rows). Return value ignored. */
typedef void (*IMG_PROGRESS_FN)(void *ctx, unsigned long done, unsigned long total);

#define IMG_OK         0
#define IMG_E_NOMEM   (-1)
#define IMG_E_SIZE    (-2)       /* empty or too large (GIF is limited to 65535 x 65535) */
#define IMG_E_CANCEL  (-3)

#define IMG_FMT_PNG  0
#define IMG_FMT_GIF  1
#define IMG_FMT_BMP  2

long ImgStride(int width);

/* Allocates a blank image (bits zeroed). Returns 0 on failure. */
int  ImgAlloc(LS_IMAGE *img, int width, int height);
void ImgFree(LS_IMAGE *img);

/* Copies the rectangle (x, y, w, h), y measured from the TOP, into dst (allocated here). */
int  ImgCrop(const LS_IMAGE *src, int x, int y, int w, int h, LS_IMAGE *dst);

int  ImgEncodeBmp(const LS_IMAGE *img, LS_BUF *out);
int  ImgEncodePng(const LS_IMAGE *img, LS_BUF *out,
                  IMG_PROGRESS_FN progress, void *ctx, volatile long *cancel);
/* Up to 256 colours are kept exactly; more are reduced with median cut (no dithering). */
int  ImgEncodeGif(const LS_IMAGE *img, LS_BUF *out,
                  IMG_PROGRESS_FN progress, void *ctx, volatile long *cancel);

int  ImgEncode(int format, const LS_IMAGE *img, LS_BUF *out,
               IMG_PROGRESS_FN progress, void *ctx, volatile long *cancel);

/* Number of distinct colours, counting stops at 257 (so 257 means "more than 256"). */
int  ImgCountColors(const LS_IMAGE *img);

void BufFree(LS_BUF *b);

#endif
