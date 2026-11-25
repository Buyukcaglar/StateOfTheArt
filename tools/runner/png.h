/* Minimal PNG writer (RGB, zlib). */
#ifndef SOTA_PNG_H
#define SOTA_PNG_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8] = { n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3] };
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32(0, h + 4, 4);
    if (n) c = crc32(c, d, n);
    uint8_t cb[4] = { c >> 24, c >> 16, c >> 8, c };
    fwrite(cb, 1, 4, f);
}
/* pixels are 0xAARRGGBB; stride in pixels */
static int png_write(const char *path, const uint32_t *px, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t raw_n = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_n), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * stride + x];
            *p++ = c >> 16; *p++ = c >> 8; *p++ = c;
        }
    }
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, raw_n, 6);
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw); free(z);
    return 0;
}
#endif
