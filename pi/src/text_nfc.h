/**
 * text_nfc.h — Compose decomposed UTF-8 (NFD) into precomposed characters
 * (NFC) just before text is drawn.
 *
 * macOS writes file names decomposed ("α" + U+0301 for "ά"), and any Matrix
 * client may send decomposed text. LVGL has no text shaping: it draws a
 * combining mark as its own glyph and cannot stack it on the letter before,
 * and our fonts carry none, so "α" + U+0301 draws as "α" and a box. Data is
 * stored and sent as is; the UI composes on the way to the screen (chat
 * bubbles in ui_chat.c, every name the library reader returns).
 */

#ifndef CHATBOX_TEXT_NFC_H
#define CHATBOX_TEXT_NFC_H

/**
 * Compose `s` in place: a base letter followed by combining marks
 * (U+0300–U+036F) becomes its precomposed letter when one exists below
 * U+0400 (é, ç, ά, ΐ, …). A mark that composes with nothing is dropped.
 * The result is never longer than the input. Malformed bytes pass through.
 */
void text_utf8_nfc(char *s);

#endif
