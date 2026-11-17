/**
 * emoji_font.h — Emoji image font using Twemoji PNGs
 *
 * Uses LVGL's imgfont system to map emoji Unicode codepoints to PNG files
 * loaded from disk (e.g., /usr/share/chatbox/emoji/1f600.png).
 */

#ifndef CHATBOX_EMOJI_FONT_H
#define CHATBOX_EMOJI_FONT_H

#include "lvgl.h"

/**
 * Initialize the emoji image font.
 * Call once after lv_init().
 *
 * @param emoji_dir  Directory containing emoji PNGs (e.g., "/usr/share/chatbox/emoji")
 */
void emoji_font_init(const char *emoji_dir);

/**
 * Get the emoji font (use as primary font on labels; Montserrat 24 is fallback).
 * Returns NULL if emoji_font_init() hasn't been called.
 */
lv_font_t *emoji_font_get(void);

/**
 * Get the size-36 emoji font (for the textarea input).
 */
lv_font_t *emoji_font_get_36(void);

/**
 * Get the emoji directory path (for loading images in the picker).
 * Returns empty string if not initialized.
 */
const char *emoji_font_get_dir(void);

#endif /* CHATBOX_EMOJI_FONT_H */
