/**
 * ui_theme.c — The shared look (see ui_theme.h).
 */

#include "ui_theme.h"

lv_color_t ui_app_color(ui_app_t app)
{
    return app == UI_APP_MUSIC ? UI_C_MUSIC : app == UI_APP_CHAT ? UI_C_CHAT
         : app == UI_APP_PHOTOS ? UI_C_PHOTOS : UI_C_INK;
}

lv_color_t ui_app_soft(ui_app_t app)
{
    return app == UI_APP_MUSIC ? UI_C_MUSIC_SOFT : app == UI_APP_CHAT ? UI_C_CHAT_SOFT
         : app == UI_APP_PHOTOS ? UI_C_PHOTOS_SOFT : UI_C_SUNK;
}

lv_color_t ui_app_ink(ui_app_t app)
{
    return app == UI_APP_MUSIC ? UI_C_MUSIC_INK : app == UI_APP_CHAT ? UI_C_CHAT_INK
         : app == UI_APP_PHOTOS ? UI_C_PHOTOS_INK : UI_C_INK;
}

lv_color_t ui_on_app(ui_app_t app)
{
    return app == UI_APP_MUSIC ? UI_C_ON_MUSIC : app == UI_APP_CHAT ? UI_C_ON_CHAT
         : app == UI_APP_PHOTOS ? UI_C_ON_PHOTOS : UI_C_SURFACE;
}

/* Ring + solid shadow, for selector `sel` (a state or 0). */
static void ring_shadow(lv_obj_t *o, int ring, int shadow, lv_style_selector_t sel)
{
    lv_obj_set_style_outline_width(o, ring, sel);
    lv_obj_set_style_outline_pad(o, 0, sel);
    lv_obj_set_style_outline_color(o, UI_C_INK, sel);
    lv_obj_set_style_outline_opa(o, ring ? LV_OPA_COVER : LV_OPA_TRANSP, sel);
    /* LVGL draws nothing for shadow_width 0; 2 with a spread is a near
     * hard edge. The spread covers the ring, the offset makes the block. */
    lv_obj_set_style_shadow_width(o, ring ? 2 : 0, sel);
    lv_obj_set_style_shadow_spread(o, ring, sel);
    lv_obj_set_style_shadow_offset_x(o, shadow, sel);
    lv_obj_set_style_shadow_offset_y(o, shadow, sel);
    lv_obj_set_style_shadow_color(o, UI_C_INK, sel);
    lv_obj_set_style_shadow_opa(o, ring ? LV_OPA_COVER : LV_OPA_TRANSP, sel);
}

void ui_item_style(lv_obj_t *o, ui_item_state_t st, ui_app_t app)
{
    if (!o) return;
    if (st == UI_ITEM_IDLE) {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_color(o, ui_app_soft(app), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    }
    bool f = st == UI_ITEM_FOCUSED;
    ring_shadow(o, f ? UI_FOCUS_RING : 0, f ? UI_FOCUS_SHADOW : 0, 0);
    lv_obj_set_style_translate_x(o, f ? -UI_FOCUS_LIFT : 0, 0);
    lv_obj_set_style_translate_y(o, f ? -UI_FOCUS_LIFT : 0, 0);
}

void ui_item_focus_states(lv_obj_t *o, ui_app_t app, bool idle_bg)
{
    if (!o) return;
    if (!idle_bg) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    ring_shadow(o, 0, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    /* FOCUSED and FOCUS_KEY both: LVGL's default theme styles FOCUS_KEY,
     * and a higher state wins over a lower one whatever the style order. */
    static const lv_style_selector_t sels[] = { LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY };
    for (unsigned i = 0; i < sizeof(sels) / sizeof(sels[0]); i++) {
        lv_obj_set_style_bg_color(o, ui_app_soft(app), sels[i]);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, sels[i]);
        lv_obj_set_style_border_width(o, 0, sels[i]);
        ring_shadow(o, UI_FOCUS_RING, UI_FOCUS_SHADOW, sels[i]);
        lv_obj_set_style_translate_x(o, -UI_FOCUS_LIFT, sels[i]);
        lv_obj_set_style_translate_y(o, -UI_FOCUS_LIFT, sels[i]);
    }
}

void ui_sticker(lv_obj_t *o, int ring, int shadow)
{
    if (o) ring_shadow(o, ring, shadow, 0);
}

lv_obj_t *ui_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                         LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

void ui_label_one_line(lv_obj_t *l, int w)
{
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

void ui_set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Bounce -------------------------------------------------------------- */

#define BOUNCE_MS   200
#define BOUNCE_AMP   14

/* v: 0..1000 → a decaying half-sine. */
static int32_t bounce_offset(int32_t v)
{
    int32_t s = lv_trigo_sin((int16_t)((v * 360) / 1000));
    return (s * BOUNCE_AMP * (1000 - v)) / (1000 * LV_TRIGO_SIN_MAX);
}

static void bounce_y_pos(void *o, int32_t v) { lv_obj_set_style_translate_y(o,  bounce_offset(v), 0); }
static void bounce_y_neg(void *o, int32_t v) { lv_obj_set_style_translate_y(o, -bounce_offset(v), 0); }
static void bounce_x_pos(void *o, int32_t v) { lv_obj_set_style_translate_x(o,  bounce_offset(v), 0); }
static void bounce_x_neg(void *o, int32_t v) { lv_obj_set_style_translate_x(o, -bounce_offset(v), 0); }

void ui_bounce(lv_obj_t *obj, ui_bounce_t kind)
{
    if (!obj || kind == UI_BOUNCE_NONE) return;
    lv_anim_exec_xcb_t exec = kind == UI_BOUNCE_UP   ? bounce_y_pos :   /* pushed down */
                              kind == UI_BOUNCE_DOWN ? bounce_y_neg :
                              kind == UI_BOUNCE_LEFT ? bounce_x_pos : bounce_x_neg;
    lv_anim_delete(obj, NULL);
    lv_obj_set_style_translate_x(obj, 0, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, BOUNCE_MS);
    lv_anim_start(&a);
}

lv_obj_t *ui_peek_create(lv_obj_t *parent)
{
    lv_obj_t *p = ui_obj(parent);
    lv_obj_set_size(p, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_max_width(p, 420, 0);
    lv_obj_set_style_radius(p, 24, 0);
    lv_obj_set_style_bg_color(p, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(p, 2, 0);
    lv_obj_set_style_outline_color(p, UI_C_INK, 0);
    lv_obj_set_style_pad_left(p, 6, 0);
    lv_obj_set_style_pad_right(p, 18, 0);
    lv_obj_set_style_pad_column(p, 10, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}
