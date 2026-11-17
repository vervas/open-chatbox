/**
 * ui_hints.h — The key-hint bar (build option CHATBOX_KEY_HINTS).
 *
 * One bar along the bottom of the screen, UI_HINTS_H tall, owned by the
 * shell: keycaps then a word ("[Enter] play"). Every context hands the
 * shell a static hint list (app_t.hints, switcher_hints()); the shell
 * shows the list of whatever is on screen. A list is identified by its
 * pointer: the bar is rebuilt only when the pointer changes.
 *
 * Without CHATBOX_KEY_HINTS the shell never creates the bar and apps get
 * the full screen (UI_APP_H == UI_H).
 */

#ifndef CHATBOX_UI_HINTS_H
#define CHATBOX_UI_HINTS_H

#include <stdbool.h>
#include "lvgl.h"

#define UI_HINT_KEYS 4

typedef struct {
    const char *keys[UI_HINT_KEYS];   /* keycap texts (LV_SYMBOL_* or words), NULL-ended */
    const char *label;
    bool        push_right;           /* this and the rest go to the right end */
} ui_hint_t;

/* Keycap texts. */
#define UI_KEY_LEFT   LV_SYMBOL_LEFT
#define UI_KEY_RIGHT  LV_SYMBOL_RIGHT
#define UI_KEY_UP     LV_SYMBOL_UP
#define UI_KEY_DOWN   LV_SYMBOL_DOWN

lv_obj_t *ui_hints_create(lv_obj_t *parent);
void      ui_hints_set(const ui_hint_t *hints, int n);

#ifdef CHATBOX_TESTING
lv_obj_t *ui_hints_bar(void);
const ui_hint_t *ui_hints_shown(int *n);
#endif

#endif /* CHATBOX_UI_HINTS_H */
