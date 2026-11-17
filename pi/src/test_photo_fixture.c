/**
 * test_photo_fixture.c — Painted test photos (see test_photo_fixture.h).
 */

#include "test_photo_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CHATBOX_HAVE_LIBJPEG
#include <jpeglib.h>
#endif
#ifdef CHATBOX_HAVE_LIBPNG
#include <png.h>
#endif

static void hsv(int hue, int s, int v, uint8_t *o)
{
    hue %= 360;
    int c = v * s / 100, x = c * (60 - abs(hue % 120 - 60)) / 60, m = v - c;
    int r, g, b;
    if (hue < 60)       { r = c; g = x; b = 0; }
    else if (hue < 120) { r = x; g = c; b = 0; }
    else if (hue < 180) { r = 0; g = c; b = x; }
    else if (hue < 240) { r = 0; g = x; b = c; }
    else if (hue < 300) { r = x; g = 0; b = c; }
    else                { r = c; g = 0; b = x; }
    o[0] = (uint8_t)((r + m) * 255 / 100);
    o[1] = (uint8_t)((g + m) * 255 / 100);
    o[2] = (uint8_t)((b + m) * 255 / 100);
}

void photo_fixture_paint(uint8_t *rgb, int w, int h, int seed)
{
    int sky = 190 + (seed * 37) % 60;          /* blues to purples */
    int sun = 30 + (seed * 53) % 30;           /* yellow-orange */
    int sx = w / 4 + (seed * 97) % (w / 2 + 1), sy = h / 4;
    int sr = (w < h ? w : h) / 7 + 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            /* Hills: two sine-ish bumps from integer maths. */
            int hx = (x * 360 / (w > 0 ? w : 1) + seed * 40) % 360;
            int bump = (hx < 180 ? hx : 360 - hx) * h / 1440;
            int hill = h * 2 / 3 - bump;
            if (y >= hill) {
                hsv(100 + (seed * 11) % 40, 70, 45 + (y - hill) * 30 / (h - hill + 1), p);
            } else {
                int dx = x - sx, dy = y - sy;
                if (dx * dx + dy * dy <= sr * sr) hsv(sun, 85, 100, p);
                else hsv(sky, 25 + y * 50 / (hill + 1), 95, p);
            }
            if (x < (w + 15) / 16) { p[0] = 230; p[1] = 40; p[2] = 40; }   /* the left stripe */
        }
    }
}

/* The stored pixel of display pixel (X, Y), for EXIF orientation o (the
 * inverse of what a viewer does). */
static void stored_xy(int o, int X, int Y, int W, int H, int *x, int *y)
{
    switch (o) {
    case 2:  *x = W - 1 - X; *y = Y;          break;
    case 3:  *x = W - 1 - X; *y = H - 1 - Y;  break;
    case 4:  *x = X;         *y = H - 1 - Y;  break;
    case 5:  *x = Y;         *y = X;          break;
    case 6:  *x = Y;         *y = W - 1 - X;  break;
    case 7:  *x = H - 1 - Y; *y = W - 1 - X;  break;
    case 8:  *x = H - 1 - Y; *y = X;          break;
    default: *x = X;         *y = Y;          break;
    }
}

bool photo_fixture_jpeg(const char *path, int w, int h, int seed, int orient)
{
#ifdef CHATBOX_HAVE_LIBJPEG
    if (orient < 1 || orient > 8) orient = 1;
    uint8_t *disp = malloc((size_t)w * h * 3);
    if (!disp) return false;
    photo_fixture_paint(disp, w, h, seed);
    bool swap = orient >= 5;
    int sw = swap ? h : w, sh = swap ? w : h;
    uint8_t *st = malloc((size_t)w * h * 3);
    if (!st) { free(disp); return false; }
    for (int Y = 0; Y < h; Y++)
        for (int X = 0; X < w; X++) {
            int x, y;
            stored_xy(orient, X, Y, w, h, &x, &y);
            memcpy(st + ((size_t)y * sw + x) * 3, disp + ((size_t)Y * w + X) * 3, 3);
        }
    free(disp);

    FILE *f = fopen(path, "wb");
    if (!f) { free(st); return false; }
    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr jerr;
    ci.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&ci);
    jpeg_stdio_dest(&ci, f);
    ci.image_width = (JDIMENSION)sw;
    ci.image_height = (JDIMENSION)sh;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, 90, TRUE);
    jpeg_start_compress(&ci, TRUE);
    if (orient != 1) {
        static uint8_t exif[] = {
            'E', 'x', 'i', 'f', 0, 0,
            'M', 'M', 0, 42, 0, 0, 0, 8,
            0, 1,                                   /* one entry */
            0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 1, 0, 0,
            0, 0, 0, 0,                             /* no next IFD */
        };
        exif[6 + 8 + 2 + 9] = (uint8_t)orient;
        jpeg_write_marker(&ci, JPEG_APP0 + 1, exif, sizeof(exif));
    }
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = st + (size_t)ci.next_scanline * sw * 3;
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    fclose(f);
    free(st);
    return true;
#else
    (void)path; (void)w; (void)h; (void)seed; (void)orient;
    return false;
#endif
}

bool photo_fixture_png(const char *path, int w, int h, int seed, bool alpha)
{
#ifdef CHATBOX_HAVE_LIBPNG
    uint8_t *rgb = malloc((size_t)w * h * 3);
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!rgb || !px) { free(rgb); free(px); return false; }
    photo_fixture_paint(rgb, w, h, seed);
    for (int i = 0; i < w * h; i++) {
        memcpy(px + i * 4, rgb + i * 3, 3);
        /* With alpha: the top half is fully transparent. */
        px[i * 4 + 3] = (alpha && i / w < h / 2) ? 0 : 255;
    }
    png_image im;
    memset(&im, 0, sizeof(im));
    im.version = PNG_IMAGE_VERSION;
    im.width = (png_uint_32)w;
    im.height = (png_uint_32)h;
    im.format = PNG_FORMAT_RGBA;
    bool ok = png_image_write_to_file(&im, path, 0, px, 0, NULL) != 0;
    free(rgb);
    free(px);
    return ok;
#else
    (void)path; (void)w; (void)h; (void)seed; (void)alpha;
    return false;
#endif
}
