/**
 * photo_decode.c — JPEG / PNG to a shown-size BGR buffer (see photo_decode.h).
 *
 * Steps: decode to a "source" buffer (JPEG: already DCT-scaled close to the
 * target; PNG: whole), centre-crop it to the target's shape (cover only),
 * box-average it to the target size in the source's own orientation, then
 * copy it out rotated/mirrored per EXIF. Upscaling (a photo smaller than
 * its box) picks the nearest source pixel.
 */

#include "photo_decode.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CHATBOX_HAVE_LIBJPEG
#include <jpeglib.h>
#endif
#ifdef CHATBOX_HAVE_LIBPNG
#include <png.h>
#endif

/* Transparent PNG pixels land on the page colour (ui_theme.h UI_C_BG). */
#define BG_R 0xD9
#define BG_G 0xE3
#define BG_B 0xED

typedef struct {
    int      w, h;
    uint8_t *px;          /* BGR, stride w * 3 */
} img_t;

/* ---- Sniffing ------------------------------------------------------------ */

photo_fmt_t photo_format_sniff(const uint8_t *d, size_t len)
{
    if (!d) return PHOTO_FMT_NONE;
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return PHOTO_FMT_JPEG;
    static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (len >= 8 && memcmp(d, png, 8) == 0) return PHOTO_FMT_PNG;
    return PHOTO_FMT_NONE;
}

photo_fmt_t photo_format_of_file(const char *path)
{
    if (!path) return PHOTO_FMT_NONE;
    FILE *f = fopen(path, "rb");
    if (!f) return PHOTO_FMT_NONE;
    uint8_t head[8];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    return photo_format_sniff(head, n);
}

bool photo_decode_supported(photo_fmt_t fmt)
{
#ifdef CHATBOX_HAVE_LIBJPEG
    if (fmt == PHOTO_FMT_JPEG) return true;
#endif
#ifdef CHATBOX_HAVE_LIBPNG
    if (fmt == PHOTO_FMT_PNG) return true;
#endif
    (void)fmt;
    return false;
}

void photo_pixels_free(photo_pixels_t *p)
{
    if (!p) return;
    free(p->px);
    p->px = NULL;
    p->w = p->h = 0;
}

/* ---- Geometry ------------------------------------------------------------ */

void photo_fit_size(int src_w, int src_h, int box_w, int box_h, photo_fit_t fit,
                    int *out_w, int *out_h)
{
    if (box_w < 1) box_w = 1;
    if (box_h < 1) box_h = 1;
    if (fit == PHOTO_FIT_COVER || src_w < 1 || src_h < 1) {
        *out_w = box_w;
        *out_h = box_h;
        return;
    }
    if ((int64_t)src_w * box_h > (int64_t)src_h * box_w) {
        *out_w = box_w;
        *out_h = (int)((int64_t)src_h * box_w / src_w);
    } else {
        *out_h = box_h;
        *out_w = (int)((int64_t)src_w * box_h / src_h);
    }
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

/* The centred crop of a w × h image to the shape tw : th (cover), or the
 * whole image (contain). */
static void crop_rect(int w, int h, int tw, int th, photo_fit_t fit,
                      int *cx, int *cy, int *cw, int *ch)
{
    *cx = *cy = 0;
    *cw = w;
    *ch = h;
    if (fit != PHOTO_FIT_COVER) return;
    if ((int64_t)w * th > (int64_t)h * tw) {          /* too wide */
        *cw = (int)((int64_t)h * tw / th);
        if (*cw < 1) *cw = 1;
        *cx = (w - *cw) / 2;
    } else {                                          /* too tall */
        *ch = (int)((int64_t)w * th / tw);
        if (*ch < 1) *ch = 1;
        *cy = (h - *ch) / 2;
    }
}

/* Box-average the crop of src to a tw × th buffer (nearest pixel where
 * the crop is smaller than the target). */
static uint8_t *resample(const img_t *src, int cx, int cy, int cw, int ch, int tw, int th)
{
    uint8_t *out = malloc((size_t)tw * (size_t)th * 3);
    int *x0 = malloc(sizeof(int) * (size_t)(tw + 1));
    uint32_t *acc = malloc(sizeof(uint32_t) * (size_t)tw * 3);
    if (!out || !x0 || !acc) {
        free(out); free(x0); free(acc);
        return NULL;
    }
    for (int ox = 0; ox <= tw; ox++)
        x0[ox] = cx + (int)((int64_t)ox * cw / tw);

    size_t stride = (size_t)src->w * 3;
    for (int oy = 0; oy < th; oy++) {
        int y0 = cy + (int)((int64_t)oy * ch / th);
        int y1 = cy + (int)((int64_t)(oy + 1) * ch / th);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src->h) y1 = src->h;
        memset(acc, 0, sizeof(uint32_t) * (size_t)tw * 3);
        for (int y = y0; y < y1; y++) {
            const uint8_t *row = src->px + (size_t)y * stride;
            for (int ox = 0; ox < tw; ox++) {
                int a = x0[ox], b = x0[ox + 1];
                if (b <= a) b = a + 1;
                if (b > src->w) b = src->w;
                uint32_t s0 = 0, s1 = 0, s2 = 0;
                for (int x = a; x < b; x++) {
                    s0 += row[x * 3];
                    s1 += row[x * 3 + 1];
                    s2 += row[x * 3 + 2];
                }
                acc[ox * 3]     += s0;
                acc[ox * 3 + 1] += s1;
                acc[ox * 3 + 2] += s2;
            }
        }
        uint8_t *o = out + (size_t)oy * tw * 3;
        int rows = y1 - y0;
        for (int ox = 0; ox < tw; ox++) {
            int a = x0[ox], b = x0[ox + 1];
            if (b <= a) b = a + 1;
            if (b > src->w) b = src->w;
            uint32_t n = (uint32_t)(rows * (b - a));
            if (n == 0) n = 1;
            o[ox * 3]     = (uint8_t)((acc[ox * 3]     + n / 2) / n);
            o[ox * 3 + 1] = (uint8_t)((acc[ox * 3 + 1] + n / 2) / n);
            o[ox * 3 + 2] = (uint8_t)((acc[ox * 3 + 2] + n / 2) / n);
        }
    }
    free(x0);
    free(acc);
    return out;
}

/* Copy the tw × th source-orientation buffer `s` to the display buffer
 * (W × H) per EXIF orientation `o`. */
static uint8_t *orient_copy(const uint8_t *s, int tw, int th, int o, int *W, int *H)
{
    bool swap = o >= 5 && o <= 8;
    *W = swap ? th : tw;
    *H = swap ? tw : th;
    uint8_t *d = malloc((size_t)tw * (size_t)th * 3);
    if (!d) return NULL;
    for (int Y = 0; Y < *H; Y++) {
        for (int X = 0; X < *W; X++) {
            int x, y;
            switch (o) {
            case 2:  x = *W - 1 - X; y = Y;           break;
            case 3:  x = *W - 1 - X; y = *H - 1 - Y;  break;
            case 4:  x = X;          y = *H - 1 - Y;  break;
            case 5:  x = Y;          y = X;           break;
            case 6:  x = Y;          y = *W - 1 - X;  break;
            case 7:  x = *H - 1 - Y; y = *W - 1 - X;  break;
            case 8:  x = *H - 1 - Y; y = X;           break;
            default: x = X;          y = Y;           break;
            }
            memcpy(d + ((size_t)Y * *W + X) * 3, s + ((size_t)y * tw + x) * 3, 3);
        }
    }
    return d;
}

/* src (source orientation) → out, for the box. */
static bool finish(const img_t *src, int orient, int box_w, int box_h, photo_fit_t fit,
                   photo_pixels_t *out)
{
    bool swap = orient >= 5 && orient <= 8;
    int dw = swap ? src->h : src->w, dh = swap ? src->w : src->h;
    int ow, oh;
    photo_fit_size(dw, dh, box_w, box_h, fit, &ow, &oh);
    int tw = swap ? oh : ow, th = swap ? ow : oh;
    int cx, cy, cw, ch;
    crop_rect(src->w, src->h, tw, th, fit, &cx, &cy, &cw, &ch);
    uint8_t *t = resample(src, cx, cy, cw, ch, tw, th);
    if (!t) return false;
    if (orient == 1) {
        out->px = t;
        out->w = tw;
        out->h = th;
        return true;
    }
    int W, H;
    uint8_t *d = orient_copy(t, tw, th, orient, &W, &H);
    free(t);
    if (!d) return false;
    out->px = d;
    out->w = W;
    out->h = H;
    return true;
}

/* ---- EXIF ---------------------------------------------------------------- */

static unsigned rd16(const uint8_t *p, bool le) { return le ? (unsigned)(p[0] | p[1] << 8) : (unsigned)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p, bool le)
{
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

int photo_exif_orientation(const uint8_t *d, size_t len)
{
    if (!d || len < 6 + 8 || memcmp(d, "Exif\0\0", 6) != 0) return 1;
    const uint8_t *t = d + 6;
    size_t tl = len - 6;
    bool le;
    if (t[0] == 'I' && t[1] == 'I') le = true;
    else if (t[0] == 'M' && t[1] == 'M') le = false;
    else return 1;
    if (rd16(t + 2, le) != 42) return 1;
    uint32_t ifd = rd32(t + 4, le);
    if (ifd > tl - 2 || ifd < 8) return 1;
    unsigned n = rd16(t + ifd, le);
    for (unsigned i = 0; i < n; i++) {
        size_t e = (size_t)ifd + 2 + (size_t)i * 12;
        if (e + 12 > tl) return 1;
        if (rd16(t + e, le) != 0x0112) continue;
        if (rd16(t + e + 2, le) != 3) return 1;          /* SHORT */
        unsigned v = rd16(t + e + 8, le);
        return v >= 1 && v <= 8 ? (int)v : 1;
    }
    return 1;
}

/* ---- JPEG ---------------------------------------------------------------- */

#ifdef CHATBOX_HAVE_LIBJPEG
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
} jerr_t;

static void jerr_exit(j_common_ptr c)
{
    jerr_t *e = (jerr_t *)c->err;
    char msg[JMSG_LENGTH_MAX];
    e->pub.format_message(c, msg);
    fprintf(stderr, "photo_decode: jpeg: %s\n", msg);
    longjmp(e->jb, 1);
}

static void jerr_quiet(j_common_ptr c, int level)
{
    (void)c; (void)level;   /* warnings (e.g. a truncated file): carry on */
}

static bool decode_jpeg(const char *path, int box_w, int box_h, photo_fit_t fit,
                        photo_pixels_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    struct jpeg_decompress_struct ci;
    jerr_t je;
    img_t src = { 0, 0, NULL };
    uint8_t *volatile buf = NULL;
    bool ok = false;

    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.emit_message = jerr_quiet;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        fclose(f);
        free(buf);
        return false;
    }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    jpeg_save_markers(&ci, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&ci, TRUE);

    int w = (int)ci.image_width, h = (int)ci.image_height;
    if (w < 1 || h < 1 || w > PHOTO_MAX_SIDE || h > PHOTO_MAX_SIDE ||
        (uint64_t)w * (uint64_t)h > PHOTO_JPEG_MAX_PIXELS) {
        fprintf(stderr, "photo_decode: %s: %dx%d is too big\n", path, w, h);
        goto done;
    }
    int orient = 1;
    for (jpeg_saved_marker_ptr m = ci.marker_list; m; m = m->next) {
        if (m->marker == JPEG_APP0 + 1) {
            orient = photo_exif_orientation(m->data, m->data_length);
            if (orient != 1) break;
        }
    }

    /* Target in source orientation, and the largest DCT scale that still
     * gives at least that many pixels across the crop. */
    bool swap = orient >= 5;
    int ow, oh;
    photo_fit_size(swap ? h : w, swap ? w : h, box_w, box_h, fit, &ow, &oh);
    int tw = swap ? oh : ow, th = swap ? ow : oh;
    int cx, cy, cw, ch;
    crop_rect(w, h, tw, th, fit, &cx, &cy, &cw, &ch);
    int denom = 1;
    for (int s = 8; s >= 2; s /= 2) {
        if ((cw + s - 1) / s >= tw && (ch + s - 1) / s >= th) { denom = s; break; }
    }
    ci.scale_num = 1;
    ci.scale_denom = (unsigned)denom;
#ifdef JCS_EXTENSIONS
    ci.out_color_space = JCS_EXT_BGR;
#else
    ci.out_color_space = JCS_RGB;
#endif
    ci.dct_method = JDCT_IFAST;
    ci.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&ci);
    if (ci.output_components != 3) goto done;

    src.w = (int)ci.output_width;
    src.h = (int)ci.output_height;
    size_t stride = (size_t)src.w * 3;
    buf = malloc(stride * (size_t)src.h);
    if (!buf) goto done;
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = buf + (size_t)ci.output_scanline * stride;
        jpeg_read_scanlines(&ci, &row, 1);
    }
#ifndef JCS_EXTENSIONS
    for (size_t i = 0; i < stride * (size_t)src.h; i += 3) {
        uint8_t r = buf[i];
        buf[i] = buf[i + 2];
        buf[i + 2] = r;
    }
#endif
    jpeg_finish_decompress(&ci);
    src.px = buf;
    ok = finish(&src, orient, box_w, box_h, fit, out);

done:
    jpeg_destroy_decompress(&ci);
    fclose(f);
    free(buf);
    return ok;
}
#endif

/* ---- PNG ----------------------------------------------------------------- */

#ifdef CHATBOX_HAVE_LIBPNG
static bool decode_png(const char *path, int box_w, int box_h, photo_fit_t fit,
                       photo_pixels_t *out)
{
    png_image im;
    memset(&im, 0, sizeof(im));
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path)) {
        fprintf(stderr, "photo_decode: png: %s\n", im.message);
        return false;
    }
    if (im.width < 1 || im.height < 1 || im.width > PHOTO_MAX_SIDE || im.height > PHOTO_MAX_SIDE ||
        (uint64_t)im.width * im.height > PHOTO_PNG_MAX_PIXELS) {
        fprintf(stderr, "photo_decode: %s: %ux%u is too big\n", path, im.width, im.height);
        png_image_free(&im);
        return false;
    }
    im.format = PNG_FORMAT_BGR;
    img_t src = { (int)im.width, (int)im.height, NULL };
    src.px = malloc(PNG_IMAGE_SIZE(im));
    if (!src.px) {
        png_image_free(&im);
        return false;
    }
    png_color bg = { BG_R, BG_G, BG_B };
    if (!png_image_finish_read(&im, &bg, src.px, 0, NULL)) {
        fprintf(stderr, "photo_decode: png: %s\n", im.message);
        png_image_free(&im);
        free(src.px);
        return false;
    }
    bool ok = finish(&src, 1, box_w, box_h, fit, out);
    free(src.px);
    return ok;
}
#endif

/* ---- Entry --------------------------------------------------------------- */

bool photo_decode(const char *path, int box_w, int box_h, photo_fit_t fit,
                  photo_pixels_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!path || box_w < 1 || box_h < 1) return false;
    photo_fmt_t fmt = photo_format_of_file(path);
#ifdef CHATBOX_HAVE_LIBJPEG
    if (fmt == PHOTO_FMT_JPEG) return decode_jpeg(path, box_w, box_h, fit, out);
#endif
#ifdef CHATBOX_HAVE_LIBPNG
    if (fmt == PHOTO_FMT_PNG) return decode_png(path, box_w, box_h, fit, out);
#endif
    (void)fmt; (void)fit;
    return false;
}
