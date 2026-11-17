/**
 * ui_theme.h — The one look every app and the switcher share
 * (docs/design.md, "the sticker kit").
 *
 * Rules (the drafts' README has the why):
 *   - Each app owns one colour: Music is orange, Chat is blue, Photos is
 *     green. It shows on
 *     the switcher tile, the app's titles, the chosen tint, progress, Play
 *     and Send.
 *   - Anything you can pick has three states (ui_item_style):
 *       idle      no background at all;
 *       selected  the app's soft tint ("this one is chosen");
 *       focused   the soft tint plus an ink ring and a small solid shadow,
 *                 lifted 2 px ("your keys are here").
 *   - Full stickers (ink ring + bigger solid shadow, ui_sticker) only for
 *     the few big things on a screen: switcher tiles, Play, Send, the
 *     volume card, the "No music yet" badge.
 *   - A dead end bounces; covers are unframed and rounded.
 *
 * Drawing cost: the ring is an outline (drawn outside the box, moves
 * nothing, like the switcher's focus always was); the solid shadow is
 * LVGL's shadow at width 2 with a spread, offset down-right. Only a handful
 * of objects carry one at a time. No opacity animations.
 *
 * Near-whites: the kiosk's HDMI panel shows v as ~(v - 16) * 255/219, so
 * anything above ~235 is pure white. UI_C_BG (#D9E3ED) shows as a pale
 * blue there; UI_C_SURFACE is white on purpose.
 */

#ifndef CHATBOX_UI_THEME_H
#define CHATBOX_UI_THEME_H

#include <stdbool.h>
#include "lvgl.h"
#include "font_chat.h"

/* ---- Screen ---------------------------------------------------------------- */

#define UI_W   1280
#define UI_H    800

/* Key hints (build option CHATBOX_KEY_HINTS): a bar along the bottom that
 * the shell owns. Apps lay out in UI_APP_H. */
#ifdef CHATBOX_KEY_HINTS
#define UI_HINTS_H   56
#else
#define UI_HINTS_H    0
#endif
#define UI_APP_H     (UI_H - UI_HINTS_H)

/* ---- Colour tokens --------------------------------------------------------- */

#define UI_C_BG          lv_color_hex(0xD9E3ED)   /* page */
#define UI_C_SURFACE     lv_color_hex(0xFFFFFF)   /* bubbles in, text box, strip, shelf */
#define UI_C_SUNK        lv_color_hex(0xC9D3DF)   /* close key, page dots */
#define UI_C_INK         lv_color_hex(0x2B3050)   /* text, rings, sticker shadows */
#define UI_C_INK_2       lv_color_hex(0x5A6283)   /* secondary text */
#define UI_C_LINE        lv_color_hex(0xA9B4C6)   /* hairlines */
#define UI_C_TRACK       lv_color_hex(0xC4CEDB)   /* progress / volume background */
#define UI_C_MUSIC       lv_color_hex(0xF28A30)
#define UI_C_MUSIC_SOFT  lv_color_hex(0xFAD9BA)
#define UI_C_MUSIC_INK   lv_color_hex(0xA3530F)   /* music titles on light */
#define UI_C_ON_MUSIC    UI_C_INK                 /* text on music orange */
#define UI_C_CHAT        lv_color_hex(0x2F88D2)
#define UI_C_CHAT_SOFT   lv_color_hex(0xC6E0F5)
#define UI_C_CHAT_INK    lv_color_hex(0x1B5F99)
#define UI_C_CHAT_PALE   lv_color_hex(0xAED3F1)   /* bubble on its way */
#define UI_C_ON_CHAT     lv_color_hex(0xFFFFFF)
#define UI_C_PHOTOS      lv_color_hex(0x2FA37A)
#define UI_C_PHOTOS_SOFT lv_color_hex(0xC3EAD9)
#define UI_C_PHOTOS_INK  lv_color_hex(0x146B4B)   /* photos titles on light */
#define UI_C_ON_PHOTOS   UI_C_INK                 /* text on photos green */
#define UI_C_SUN         lv_color_hex(0xFFD23F)   /* the "Now" sticker */
#define UI_C_ALERT       lv_color_hex(0xE0474C)   /* late bubble, unread dot, New */
#define UI_C_ON_ALERT    lv_color_hex(0xFFFFFF)

/* ---- Shape ----------------------------------------------------------------- */

#define UI_RADIUS         20
#define UI_RADIUS_BIG     40    /* switcher tiles */
#define UI_FOCUS_RING      3
#define UI_FOCUS_SHADOW    5
#define UI_FOCUS_LIFT      2
#define UI_HEAD_H         68    /* an app's own header */

typedef enum { UI_APP_NONE = 0, UI_APP_MUSIC, UI_APP_CHAT, UI_APP_PHOTOS } ui_app_t;

lv_color_t ui_app_color(ui_app_t app);
lv_color_t ui_app_soft(ui_app_t app);
lv_color_t ui_app_ink(ui_app_t app);
lv_color_t ui_on_app(ui_app_t app);

typedef enum { UI_ITEM_IDLE = 0, UI_ITEM_SELECTED, UI_ITEM_FOCUSED } ui_item_state_t;

/* Put object `o` in state `st` (local styles; for items whose focus the
 * app tracks itself: music rows and cards, switcher tiles). Cheap when the
 * state is unchanged: LVGL skips equal style values. */
void ui_item_style(lv_obj_t *o, ui_item_state_t st, ui_app_t app);

/* The same look on LVGL's own focus (LV_STATE_FOCUSED): for widgets in a
 * focus group (chat's contacts, text box, emoji). `idle_bg` is the
 * unfocused background (LV_OPA_TRANSP for none). */
void ui_item_focus_states(lv_obj_t *o, ui_app_t app, bool idle_bg);

/* A full sticker: an ink ring of `ring` px and a solid ink shadow `shadow`
 * px down-right. ring = 0 clears both. */
void ui_sticker(lv_obj_t *o, int ring, int shadow);

/* A plain object: no theme styles, not scrollable, not clickable. */
lv_obj_t *ui_obj(lv_obj_t *parent);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

/* Label on one line, cut with "..." at width w. */
void ui_label_one_line(lv_obj_t *l, int w);

/* Hide or show `o` (NULL: nothing). */
void ui_set_hidden(lv_obj_t *o, bool hidden);

/* The edge bounce: feedback for a key that has nowhere to go. `obj` (a
 * list, a photo) is pushed 14 px the way the key points and springs back
 * in 200 ms, a translate only (nothing is laid out again). UP pushes it
 * down, as if the top edge was hit. NULL: nothing. */
typedef enum {
    UI_BOUNCE_NONE = 0, UI_BOUNCE_UP, UI_BOUNCE_DOWN, UI_BOUNCE_LEFT, UI_BOUNCE_RIGHT
} ui_bounce_t;
void ui_bounce(lv_obj_t *obj, ui_bounce_t kind);

/* The small pill in an app's header where the other app peeks in (a
 * contact's face in Music, the playing song in Chat). Empty and hidden; the
 * shell fills it (shell_attach_peek). */
lv_obj_t *ui_peek_create(lv_obj_t *parent);

#endif /* CHATBOX_UI_THEME_H */
