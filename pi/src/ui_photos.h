/**
 * ui_photos.h — Photos app UI (docs/photos.md; the sticker kit, ui_theme.h;
 * docs/design.md).
 *
 * Views (one at a time, inside the app's root):
 *   EMPTY    "No photos yet": the folder is missing or has no photos.
 *   GRID     every photo, newest first, four across, each cropped to a 4:3
 *            card with its date under it.
 *   VIEWER   one photo, whole, as large as fits (a 3:1 panorama is
 *            letterboxed), with ← / → arrows and "N of M".
 * The delete confirm sticker can show over the grid or the viewer.
 *
 * Keys arrive already filtered (shell_filter_key with KEY_CTX_PHOTOS) from
 * app_photos.c's key sink:
 *   grid     arrows move (edges bounce), Enter opens the viewer, Delete asks
 *            to delete the selected photo;
 *   viewer   ← / → previous / next (ends bounce), Delete asks to delete it;
 *   confirm  Enter deletes (the file only; the chat keeps its bubble), Esc
 *            keeps it; every other key is ignored.
 * Esc comes through ui_photos_handle_esc(): it closes the confirm sticker,
 * or the viewer (back to the grid); on the grid it isn't ours (the shell
 * opens the switcher).
 *
 * Threading: LVGL thread only.
 */

#ifndef CHATBOX_UI_PHOTOS_H
#define CHATBOX_UI_PHOTOS_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "key_filter.h"
#include "photo_lib.h"
#include "ui_hints.h"

typedef enum {
    PHOTOS_VIEW_EMPTY = 0,
    PHOTOS_VIEW_GRID,
    PHOTOS_VIEW_VIEWER,
} photos_view_t;

enum { PHOTOS_BOUNCE_NONE = 0, PHOTOS_BOUNCE_UP, PHOTOS_BOUNCE_DOWN, PHOTOS_BOUNCE_LEFT,
       PHOTOS_BOUNCE_RIGHT };

/* Build every widget inside `root` (hidden views). */
void ui_photos_create(lv_obj_t *root);

/* The library (NULL: "No photos yet"). Before or after create(). */
void ui_photos_set_library(photo_lib_t *lib);

/* Redraw from the library now (after a scan found a change), keeping the
 * selection on the same photo when it is still there. */
void ui_photos_reload(void);

key_ctx_t ui_photos_key_ctx(void);
void ui_photos_handle_key(uint32_t key);
bool ui_photos_handle_esc(void);

void ui_photos_on_show(void);
void ui_photos_on_hide(void);

photos_view_t ui_photos_view(void);
bool ui_photos_confirm_open(void);

/* Live tile (T9): the newest photo and how many there are, or a big
 * picture glyph when there are none. */
void ui_photos_tile_render(lv_obj_t *face);
void ui_photos_set_tile_listener(void (*cb)(void));

const ui_hint_t *ui_photos_hints(int *n);

#ifdef CHATBOX_TESTING
typedef struct {
    photos_view_t view;
    int   count;
    int   sel;
    bool  confirm_open;
    char  head_sub[48];          /* "13 photos" */
    bool  empty_visible;
    bool  grid_visible;
    bool  viewer_visible;
    int   cards_bound;
    int   cards_image;           /* bound cards showing their photo */
    int   cards_failed;          /* ...showing the can't-show placeholder */
    int   cards_loading;
    bool  sel_card_focused;
    char  sel_caption[64];
    char  viewer_title[64];
    char  viewer_count[32];      /* "3 of 13" */
    bool  viewer_image;
    bool  viewer_failed;
    int   viewer_img_w, viewer_img_h;
    int   viewer_decodes;        /* photos the viewer has decoded so far */
    bool  arrow_left_dim, arrow_right_dim;
    int   bounces;
    int   last_bounce;
    bool  tile_built, tile_image, tile_empty;
    char  tile_label[48];
} photos_test_state_t;
void ui_photos_test_state(photos_test_state_t *out);
/* Decode every thumbnail still waiting (they normally load one per tick). */
void ui_photos_test_flush(void);
#endif

#endif /* CHATBOX_UI_PHOTOS_H */
