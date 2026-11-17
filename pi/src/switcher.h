/**
 * switcher.h — Full-screen app switcher overlay (docs/player-spec.md §6, §8.5).
 *
 * UI and key handling only. The shell (shell.c) decides when the switcher
 * is open and which group the keyboard uses; the switcher calls back into
 * the shell to close itself or to switch app.
 */

#ifndef CHATBOX_SWITCHER_H
#define CHATBOX_SWITCHER_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_hints.h"
#include "ui_theme.h"

#define SWITCHER_MAX_TILES 4

/* One tile: the app's id, its bold caption and its colours. */
typedef struct {
    const char *id;
    const char *label;
    ui_app_t    look;
} switcher_tile_info_t;

/* Build the overlay (hidden) as the last child of `parent`, with one tile
 * per entry, left to right. Must be called with the default group
 * suspended (shell_start does this). */
void switcher_create(lv_obj_t *parent, const switcher_tile_info_t *tiles, int count);

/* Key hints for the switcher (CHATBOX_KEY_HINTS; ui_hints.h). */
const ui_hint_t *switcher_hints(int *n);

/* Delete the overlay and its group (tests / shutdown). */
void switcher_destroy(void);

lv_group_t *switcher_group(void);

/* The live-tile area of app `id`'s tile (NULL for an unknown id): what the
 * shell hands to app->tile_render(). A fixed box above the caption; its
 * position doesn't depend on focus or NOW. */
lv_obj_t *switcher_tile_face(const char *id);

/* Show the overlay with `current_id`'s tile marked NOW and focused, and
 * start the fly-in. Does not touch indevs: the shell does. */
void switcher_open(const char *current_id);

/* Hide the overlay at once, stopping every animation (no completion). */
void switcher_close(void);

/* Jump any running animation to its end state. A finished zoom completes
 * its app switch (shell_switch_to). Cheap no-op when nothing runs. */
void switcher_finish_anims(void);

/* Act on an already-filtered key (§8.5). Returns true when the key is one
 * the switcher uses. */
bool switcher_handle_key(uint32_t key);

#ifdef CHATBOX_TESTING
lv_obj_t *switcher_get_root(void);
lv_obj_t *switcher_get_tile(int idx);
lv_obj_t *switcher_get_now_tag(int idx);
lv_obj_t *switcher_get_caption(int idx);
bool switcher_clock_running(void);
int  switcher_get_focus(void);
int  switcher_get_current(void);
int  switcher_get_shake_count(void);
int  switcher_get_bounce_count(void);
bool switcher_is_animating(void);
bool switcher_is_zooming(void);
#endif

#endif /* CHATBOX_SWITCHER_H */
