/**
 * photo_decode.h — Decode a JPEG or PNG photo straight to the size it is
 * shown at (docs/photos.md).
 *
 * Photos come from phones: 12 MP and more. Decoding one at full size into
 * LVGL's 16 MB heap (or its 5 MB image cache) is not an option, so the UI
 * never hands a photo file to LVGL. It asks for the pixels at the size it
 * draws them, and gets a small BGR buffer (LVGL's RGB888 byte order) in
 * ordinary malloc'd memory that it wraps in an lv_image_dsc_t:
 *
 *   - JPEG (libjpeg / libjpeg-turbo): the decoder's own DCT scaling (1/2,
 *     1/4, 1/8) does most of the shrinking, so a 4000×3000 photo for a
 *     256×192 card is decoded at 500×375 and then box-averaged down. EXIF
 *     orientation is applied (phones store portraits sideways). Baseline
 *     and progressive; grey, YCbCr and RGB. CMYK is refused.
 *   - PNG (libpng): decoded whole (the format has no cheap scaling),
 *     transparent pixels composited on the page colour, then averaged
 *     down. Capped at PHOTO_PNG_MAX_PIXELS.
 *
 * Fits:
 *   PHOTO_FIT_COVER    exactly box_w × box_h, the photo centre-cropped to
 *                      that shape (the grid's 4:3 cards, chat bubbles).
 *   PHOTO_FIT_CONTAIN  the whole photo, as large as fits in the box, its
 *                      shape kept (the viewer: a 3:1 panorama letterboxed).
 *
 * Bad data never crashes: anything wrong (missing file, not a JPEG/PNG,
 * truncated header, absurd dimensions, decoder error) returns false. A file
 * truncated after its header decodes with the missing part grey (libjpeg's
 * behaviour), which is what a photo still being copied in looks like until
 * the library rescans it.
 *
 * Either library is optional at build time (CHATBOX_HAVE_LIBJPEG,
 * CHATBOX_HAVE_LIBPNG): without it that format never decodes and the UI
 * shows its placeholder. photo_format_sniff() always works.
 *
 * Pure C, no LVGL. Blocking (it is called on a key press or from a
 * one-photo-per-tick timer, never in a tight loop). Thread-agnostic.
 */

#ifndef CHATBOX_PHOTO_DECODE_H
#define CHATBOX_PHOTO_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHOTO_MAX_SIDE         20000                 /* either dimension, from the header */
#define PHOTO_JPEG_MAX_PIXELS  (120u * 1000u * 1000u) /* header w×h (DCT-scaled before decoding) */
#define PHOTO_PNG_MAX_PIXELS   (24u * 1000u * 1000u)  /* PNGs are decoded whole */

typedef enum { PHOTO_FMT_NONE = 0, PHOTO_FMT_JPEG, PHOTO_FMT_PNG } photo_fmt_t;
typedef enum { PHOTO_FIT_COVER = 0, PHOTO_FIT_CONTAIN } photo_fit_t;

typedef struct {
    int      w, h;        /* stride is w * 3 */
    uint8_t *px;          /* B, G, R per pixel; malloc'd, photo_pixels_free() */
} photo_pixels_t;

/* The format from the first bytes (magic numbers, not the name). */
photo_fmt_t photo_format_sniff(const uint8_t *data, size_t len);
/* ...of a file (reads its first bytes). */
photo_fmt_t photo_format_of_file(const char *path);

/* Whether this build can decode `fmt`. */
bool photo_decode_supported(photo_fmt_t fmt);

/* Decode `path` for a box_w × box_h box (see the fits above). On success
 * fills *out (free it with photo_pixels_free) and returns true. */
bool photo_decode(const char *path, int box_w, int box_h, photo_fit_t fit,
                  photo_pixels_t *out);

void photo_pixels_free(photo_pixels_t *p);

/* The output size for a src_w × src_h photo (after EXIF rotation) in a
 * box_w × box_h box. Exposed for tests. */
void photo_fit_size(int src_w, int src_h, int box_w, int box_h, photo_fit_t fit,
                    int *out_w, int *out_h);

/* EXIF orientation (1..8) from a JPEG APP1 payload ("Exif\0\0" + TIFF);
 * 1 when absent or unreadable. Exposed for tests. */
int photo_exif_orientation(const uint8_t *app1, size_t len);

#endif /* CHATBOX_PHOTO_DECODE_H */
