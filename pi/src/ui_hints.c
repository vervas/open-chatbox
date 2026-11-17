/**
 * ui_hints.c — The key-hint bar (see ui_hints.h).
 */

#include "ui_hints.h"
#include "ui_theme.h"

#include <string.h>

static lv_obj_t        *g_bar;
static const ui_hint_t *g_shown;
static int              g_shown_n;

static void bar_delete_cb(lv_event_t *e)
{
    (void)e;
    g_bar = NULL;
    g_shown = NULL;
    g_shown_n = 0;
}

lv_obj_t *ui_hints_create(lv_obj_t *parent)
{
    g_bar = ui_obj(parent);
    lv_obj_set_size(g_bar, UI_W, UI_HINTS_H > 0 ? UI_HINTS_H : 56);
    lv_obj_align(g_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(g_bar, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(g_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(g_bar, 2, 0);
    lv_obj_set_style_border_color(g_bar, UI_C_SUNK, 0);
    lv_obj_set_style_pad_hor(g_bar, 24, 0);
    lv_obj_set_style_pad_column(g_bar, 36, 0);
    lv_obj_set_flex_flow(g_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(g_bar, bar_delete_cb, LV_EVENT_DELETE, NULL);
    g_shown = NULL;
    g_shown_n = 0;
    return g_bar;
}

/* A keycap: an outlined face on a slightly deeper base, everything inside
 * its own box (an outline or offset shadow would be clipped by the row).
 *   base  KEY_H tall, ink-2, the face's colour showing KEY_DEPTH at the bottom
 *   face  KEY_H - KEY_DEPTH tall, page colour, a KEY_LINE ink-2 border */
#define KEY_H      40
#define KEY_DEPTH   4
#define KEY_LINE    2
#define KEY_RADIUS  9

static void keycap(lv_obj_t *parent, const char *text)
{
    lv_obj_t *base = ui_obj(parent);
    lv_obj_set_size(base, LV_SIZE_CONTENT, KEY_H);
    lv_obj_set_style_radius(base, KEY_RADIUS, 0);
    lv_obj_set_style_bg_color(base, UI_C_INK_2, 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);

    lv_obj_t *face = ui_obj(base);
    lv_obj_set_size(face, LV_SIZE_CONTENT, KEY_H - KEY_DEPTH);
    lv_obj_set_style_min_width(face, KEY_H, 0);
    lv_obj_set_style_pad_hor(face, 10, 0);
    lv_obj_set_style_radius(face, KEY_RADIUS, 0);
    lv_obj_set_style_bg_color(face, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face, KEY_LINE, 0);
    lv_obj_set_style_border_color(face, UI_C_INK_2, 0);

    /* LV_SYMBOL_* live in the Montserrat symbol range (U+F000..). */
    bool sym = (unsigned char)text[0] == 0xEF;
    lv_obj_t *l = ui_label(face, sym ? &lv_font_montserrat_24 : &font_chat_28, UI_C_INK);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

void ui_hints_set(const ui_hint_t *hints, int n)
{
    if (!g_bar || (hints == g_shown && n == g_shown_n)) return;
    g_shown = hints;
    g_shown_n = n;
    lv_obj_clean(g_bar);
    for (int i = 0; i < n; i++) {
        lv_obj_t *h = ui_obj(g_bar);
        lv_obj_set_size(h, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_column(h, 6, 0);
        lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (hints[i].push_right) lv_obj_set_flex_grow(h, 1);
        if (hints[i].push_right) lv_obj_set_style_flex_main_place(h, LV_FLEX_ALIGN_END, 0);
        for (int k = 0; k < UI_HINT_KEYS && hints[i].keys[k]; k++) keycap(h, hints[i].keys[k]);
        lv_obj_t *l = ui_label(h, &font_chat_28, UI_C_INK_2);
        lv_obj_set_style_pad_left(l, 6, 0);
        lv_label_set_text(l, hints[i].label);
    }
}

#ifdef CHATBOX_TESTING
lv_obj_t *ui_hints_bar(void)
{
    return g_bar;
}

const ui_hint_t *ui_hints_shown(int *n)
{
    if (n) *n = g_shown_n;
    return g_shown;
}
#endif
