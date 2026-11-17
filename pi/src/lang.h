/**
 * lang.h — per-contact typing language (keyboard layout + accent combo)
 *
 * A contact can carry a language (contact.N.lang = gr | fr). Opening that
 * contact's chat loads the language's XKB layout on the Pi keyboard (Greek
 * letters for gr; fr keeps the default US QWERTY) and turns on the accent
 * combo in the chat textarea: type ` then a vowel and the pair becomes the
 * accented letter (Greek: ` α -> ά, French: ` e -> é). Two ` give the grave
 * accent (French: `` e -> è), on all five vowels. The ` shows as soon as
 * it is typed; any other key leaves it as a plain `.
 *
 * Pure logic, no LVGL: ui_chat.c applies the combo, main.c the layout.
 *
 * The layout is a single XKB layout today. A later Greek/English toggle is
 * a layout list ("gr,us") plus an XKB group-switch option; the accent
 * table would then follow the active XKB group.
 */

#ifndef CHATBOX_LANG_H
#define CHATBOX_LANG_H

#include <stdint.h>

typedef enum {
    LANG_NONE = 0,      /* default keyboard, no accent combo */
    LANG_GREEK,
    LANG_FRENCH,
    LANG_COUNT
} chat_lang_t;

/* The key typed ahead of a vowel to accent it. */
#define LANG_ACCENT_KEY '`'

/* "gr" / "el" / "greek" -> LANG_GREEK, "fr" / "french" -> LANG_FRENCH,
 * "" / "none" -> LANG_NONE (case-insensitive). Returns -1 for anything
 * else so the config parser can warn. */
int lang_from_name(const char *name);

/* Config name ("gr", "fr", "none"). */
const char *lang_name(chat_lang_t lang);

/* XKB layout to load while this language's chat is open ("gr"), or NULL
 * to use the default keymap (LV_LIBINPUT_XKB_KEY_MAP). */
const char *lang_xkb_layout(chat_lang_t lang);

/**
 * The accent combo, for a textarea's insert hook. `text` is the current
 * text, `cursor` the cursor position in characters, `inserted` the UTF-8
 * about to be inserted at the cursor. If the character before the cursor is
 * LANG_ACCENT_KEY and `inserted` is one vowel the language accents, returns
 * the accented letter and sets *ticks to how many ` before the cursor it
 * used (2 for a grave from ``, else 1): the caller deletes those and inserts
 * the letter instead. Otherwise NULL (insert as typed; *ticks untouched).
 */
const char *lang_accent_combo(chat_lang_t lang, const char *text,
                              uint32_t cursor, const char *inserted,
                              uint32_t *ticks);

#endif /* CHATBOX_LANG_H */
