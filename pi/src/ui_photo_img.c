/**
 * ui_photo_img.c — A decoded photo as an LVGL image source (see the header).
 */

#include "ui_photo_img.h"

#include <string.h>

static const uint8_t k_blank_px[4] = { 0, 0, 0, 0 };
static const lv_image_dsc_t k_blank = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                .w = 1, .h = 1, .stride = 4 },
    .data_size = 4,
    .data = k_blank_px,
};

void ui_photo_img_unset(lv_obj_t *img)
{
    if (img && lv_image_get_src(img) != &k_blank) lv_image_set_src(img, &k_blank);
}

bool ui_photo_img_ok(const ui_photo_img_t *im)
{
    return im && im->px.px != NULL;
}

void ui_photo_img_free(ui_photo_img_t *im)
{
    if (!im) return;
    if (im->px.px) {
        lv_image_cache_drop(&im->dsc);
        photo_pixels_free(&im->px);
    }
    memset(im, 0, sizeof(*im));
}

bool ui_photo_img_load(ui_photo_img_t *im, const char *path, int w, int h, photo_fit_t fit)
{
    if (!im) return false;
    ui_photo_img_free(im);
    if (!path || !photo_decode(path, w, h, fit, &im->px)) return false;
    im->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    im->dsc.header.cf     = LV_COLOR_FORMAT_RGB888;
    im->dsc.header.w      = (uint32_t)im->px.w;
    im->dsc.header.h      = (uint32_t)im->px.h;
    im->dsc.header.stride = (uint32_t)im->px.w * 3;
    im->dsc.data_size     = (uint32_t)(im->px.w * im->px.h * 3);
    im->dsc.data          = im->px.px;
    return true;
}
