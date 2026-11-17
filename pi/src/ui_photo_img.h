/**
 * ui_photo_img.h — A decoded photo as an LVGL image source (photos grid,
 * viewer, switcher tile, chat bubbles).
 *
 * The pixels are photo_decode.h's shown-size BGR buffer in ordinary
 * malloc'd memory, outside LVGL's 16 MB heap, wrapped in an
 * lv_image_dsc_t (RGB888) that an lv_image can take as its source. Drawn
 * 1:1, so no transform runs per frame.
 *
 * Freeing: set every lv_image that shows it to another source first, then
 * ui_photo_img_free() drops LVGL's cache entries for the descriptor (a
 * later image at the same address must not be drawn from a stale entry)
 * and frees the pixels.
 */

#ifndef CHATBOX_UI_PHOTO_IMG_H
#define CHATBOX_UI_PHOTO_IMG_H

#include <stdbool.h>
#include "lvgl.h"
#include "photo_decode.h"

typedef struct {
    lv_image_dsc_t dsc;
    photo_pixels_t px;
} ui_photo_img_t;

/* Decode `path` for a w × h box (photo_decode). Frees what `im` held
 * first. False (and `im` empty) when the photo doesn't decode. */
bool ui_photo_img_load(ui_photo_img_t *im, const char *path, int w, int h, photo_fit_t fit);

bool ui_photo_img_ok(const ui_photo_img_t *im);

void ui_photo_img_free(ui_photo_img_t *im);

/* Point lv_image `img` at a blank 1×1 source (before freeing what it
 * showed). lv_image_set_src(img, NULL) would do but logs a warning. */
void ui_photo_img_unset(lv_obj_t *img);

#endif /* CHATBOX_UI_PHOTO_IMG_H */
