/**
 * emoji_font.c — Emoji image font using Twemoji PNGs
 *
 * Maps emoji Unicode codepoints to 24x24 PNG files via LVGL's imgfont system.
 * Non-emoji characters fall through to the chat font (font_chat_*) via the fallback font.
 */

#include "emoji_font.h"
#include "font_chat.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Maximum path length for emoji file paths */
#define EMOJI_PATH_MAX 256

/* Static state */
static lv_font_t *g_emoji_font = NULL;
static lv_font_t *g_emoji_font_36 = NULL;
static char        g_emoji_dir[EMOJI_PATH_MAX] = {0};
static int         g_emoji_offset_y = 0;

/**
 * imgfont callback: return the file path for a given Unicode codepoint.
 * Returns "A:{dir}/{codepoint}.png" if the file exists, NULL otherwise.
 */
static const void *emoji_path_cb(const lv_font_t *font, uint32_t unicode,
                                  uint32_t unicode_next, int32_t *offset_y,
                                  void *user_data)
{
    (void)font;
    (void)unicode_next;
    (void)user_data;

    /* Only handle codepoints in emoji ranges */
    if (unicode < 0x2600) return NULL;

    /* Build the raw filesystem path first to check existence */
    static char raw_path[EMOJI_PATH_MAX];
    snprintf(raw_path, sizeof(raw_path), "%s/%04x.png", g_emoji_dir, unicode);

    if (access(raw_path, F_OK) != 0) return NULL;

    /* Build the LVGL FS path with drive letter prefix */
    static char lv_path[EMOJI_PATH_MAX];
    snprintf(lv_path, sizeof(lv_path), "A:%s/%04x.png", g_emoji_dir, unicode);

    /* Compensate for base_line shift so emojis stay vertically centered.
     * Only apply to the chat font — the textarea font needs no adjustment. */
    if (offset_y) *offset_y = (font == g_emoji_font) ? g_emoji_offset_y : 0;

    return lv_path;
}

void emoji_font_init(const char *emoji_dir)
{
    if (!emoji_dir) return;

    strncpy(g_emoji_dir, emoji_dir, sizeof(g_emoji_dir) - 1);
    g_emoji_dir[sizeof(g_emoji_dir) - 1] = '\0';

    g_emoji_font = lv_imgfont_create(48, emoji_path_cb, NULL);
    if (!g_emoji_font) {
        fprintf(stderr, "emoji_font: failed to create imgfont\n");
        return;
    }

    /* Fall back to Montserrat 36 for regular text characters (chat bubbles).
     * Center the fallback glyphs vertically within the 48px imgfont line. */
    g_emoji_font->fallback = &font_chat_36;
    int extra = g_emoji_font->line_height - font_chat_36.line_height;
    g_emoji_font->base_line = font_chat_36.base_line + extra / 2;
    g_emoji_offset_y = -(extra + 8);

    /* Second imgfont for the textarea — same PNG size, different text fallback */
    g_emoji_font_36 = lv_imgfont_create(48, emoji_path_cb, NULL);
    if (g_emoji_font_36) {
        g_emoji_font_36->fallback = &font_chat_48;
        int extra2 = g_emoji_font_36->line_height - font_chat_48.line_height;
        if (extra2 > 0) {
            g_emoji_font_36->base_line = font_chat_48.base_line + extra2 / 2;
        } else {
            /* The text font is the taller one (60 px): take its metrics, or
             * descenders (g, y, η) fall below the 48 px line and get clipped */
            g_emoji_font_36->line_height = font_chat_48.line_height;
            g_emoji_font_36->base_line   = font_chat_48.base_line;
        }
    }

    fprintf(stderr, "emoji_font: initialized from %s\n", emoji_dir);
}

lv_font_t *emoji_font_get(void)
{
    return g_emoji_font;
}

lv_font_t *emoji_font_get_36(void)
{
    return g_emoji_font_36;
}

const char *emoji_font_get_dir(void)
{
    return g_emoji_dir;
}
