#ifndef FONT_CHAT_H
#define FONT_CHAT_H

#include "lvgl.h"

/* Reading text: Didact Gothic (+ DejaVu Sans for the few glyphs it lacks).
 * Titles: M PLUS Rounded 1c ExtraBold. Regenerate with `make fonts`. */
extern const lv_font_t font_chat_28;
extern const lv_font_t font_chat_36;
extern const lv_font_t font_chat_48;
extern const lv_font_t font_title_28;
extern const lv_font_t font_title_36;
extern const lv_font_t font_title_48;
/* Big glyphs drawn at size (scaling blurs): the music note (LV_SYMBOL_AUDIO
 * only) and digits + "+" for the Chat tile's count. */
extern const lv_font_t font_icon_120;
extern const lv_font_t font_count_96;

#endif /* FONT_CHAT_H */
