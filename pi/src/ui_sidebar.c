/**
 * ui_sidebar.c — Contact sidebar with avatar thumbnails
 */

#include "ui_sidebar.h"
#include "ui_theme.h"
#include "avatars.h"
#include <stdint.h>
#include <stdio.h>

/* The shelf beside an open chat: one avatar per contact (a chat is always
 * open: the kiosk boots into the first contact's). */
#define SIDEBAR_W           128
#define AVATAR_SIZE          88   /* the 128 px PNG, centred in a circle */
#define DOT_SIZE             22

/* The contacts sit on a white shelf (rounded, the page around it: see
 * app_chat.c). A row is idle (nothing), selected (the contact you're
 * talking to: chat's soft tint) or focused (tint + ink ring + solid
 * shadow, ui_theme.h). */
#define SHELF_RADIUS     28

static void (*g_on_select)(int contact_idx) = NULL;
static lv_obj_t *g_sidebar = NULL;
static lv_obj_t *g_rows[CONFIG_MAX_CONTACTS];
static lv_obj_t *g_avatars[CONFIG_MAX_CONTACTS];
static const char *g_names[CONFIG_MAX_CONTACTS];   /* the config's, kept for the process */
static lv_obj_t *g_dots[CONFIG_MAX_CONTACTS];
/* Unread state: the one record the dots and the switcher's Chat tile both
 * show. 0 = read; otherwise the order of the contact's latest unread
 * message (higher = newer), so the tile can show the newest sender. */
static uint32_t g_unread_seq[CONFIG_MAX_CONTACTS];
static uint32_t g_seq = 0;
static void (*g_unread_listener)(void) = NULL;
static int g_count = 0;
static int g_selected = -1;
static bool g_chat_active = false;

static void row_click_cb(lv_event_t *e);
static void row_focus_cb(lv_event_t *e);
static void next_contact_async(void *unused);
static bool g_next_pending;

void ui_sidebar_create(lv_obj_t *parent, const config_t *cfg,
                       void (*on_select)(int contact_idx))
{
    g_on_select = on_select;
    g_sidebar = parent;
    g_count = cfg->contact_count;
    if (g_count > CONFIG_MAX_CONTACTS) g_count = CONFIG_MAX_CONTACTS;
    for (int i = 0; i < CONFIG_MAX_CONTACTS; i++) g_unread_seq[i] = 0;
    g_seq = 0;
    g_selected = -1;
    g_chat_active = false;
    if (g_next_pending) lv_async_call_cancel(next_contact_async, NULL);
    g_next_pending = false;

    lv_obj_set_width(parent, SIDEBAR_W);
    lv_obj_set_height(parent, lv_pct(100));
    lv_obj_set_style_bg_color(parent, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, SHELF_RADIUS, 0);
    lv_obj_set_style_pad_all(parent, 14, 0);
    lv_obj_set_style_pad_gap(parent, 12, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Keyboard navigation group */
    lv_group_t *grp = lv_group_create();
    lv_group_set_default(grp);

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
            lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, grp);
        }
        indev = lv_indev_get_next(indev);
    }

    for (int i = 0; i < cfg->contact_count; i++) {
        /* Row container */
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, AVATAR_SIZE + 12);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        ui_item_focus_states(row, UI_APP_CHAT, false);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* the focus ring */
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, NULL);
        lv_group_add_obj(grp, row);
        g_rows[i] = row;

        /* Avatar image */
        const void *av = avatar_get(cfg->contacts[i].avatar);
        lv_obj_t *img = lv_image_create(row);
        if (av) lv_image_set_src(img, av);
        lv_obj_set_size(img, AVATAR_SIZE, AVATAR_SIZE);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_style_radius(img, AVATAR_SIZE / 2, 0);
        lv_obj_set_style_clip_corner(img, true, 0);
        g_avatars[i] = img;

        /* Unread dot — a red sticker on the top-right of the avatar */
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, DOT_SIZE / 2, 0);
        lv_obj_set_style_bg_color(dot, UI_C_ALERT, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_outline_width(dot, 2, 0);
        lv_obj_set_style_outline_color(dot, UI_C_INK, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
        lv_obj_align_to(dot, img, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        g_dots[i] = dot;
        g_names[i] = cfg->contacts[i].name;
    }
}

void ui_sidebar_set_selected(int contact_idx)
{
    /* Clear previous selection */
    if (g_selected >= 0 && g_selected < g_count) {
        lv_obj_set_style_bg_opa(g_rows[g_selected], LV_OPA_TRANSP, 0);
    }

    g_selected = contact_idx;

    /* Highlight new selection */
    if (g_selected >= 0 && g_selected < g_count) {
        lv_obj_set_style_bg_color(g_rows[g_selected], UI_C_CHAT_SOFT, 0);
        lv_obj_set_style_bg_opa(g_rows[g_selected], LV_OPA_COVER, 0);
    }
}

void ui_sidebar_set_unread(int contact_idx, bool unread)
{
    if (contact_idx < 0 || contact_idx >= g_count) return;
    if (unread) {
        /* Every new message makes its sender the newest, even when the
         * contact was already unread. */
        g_unread_seq[contact_idx] = ++g_seq;
        lv_obj_clear_flag(g_dots[contact_idx], LV_OBJ_FLAG_HIDDEN);
    } else {
        if (g_unread_seq[contact_idx] == 0) return;
        g_unread_seq[contact_idx] = 0;
        lv_obj_add_flag(g_dots[contact_idx], LV_OBJ_FLAG_HIDDEN);
    }
    if (g_unread_listener) g_unread_listener();
}

int ui_sidebar_unread_count(void)
{
    int n = 0;
    for (int i = 0; i < g_count; i++) n += g_unread_seq[i] != 0;
    return n;
}

int ui_sidebar_newest_unread(void)
{
    int best = -1;
    for (int i = 0; i < g_count; i++) {
        if (g_unread_seq[i] && (best < 0 || g_unread_seq[i] > g_unread_seq[best]))
            best = i;
    }
    return best;
}

const void *ui_sidebar_avatar_src(int contact_idx)
{
    if (contact_idx < 0 || contact_idx >= g_count) return NULL;
    return lv_image_get_src(g_avatars[contact_idx]);
}

const char *ui_sidebar_contact_name(int contact_idx)
{
    if (contact_idx < 0 || contact_idx >= g_count) return "";
    return g_names[contact_idx];
}

void ui_sidebar_set_unread_listener(void (*cb)(void))
{
    g_unread_listener = cb;
}

void ui_sidebar_open(int contact_idx)
{
    if (contact_idx < 0 || contact_idx >= g_count) return;
    g_chat_active = true;   /* from now on a Tab onto a row = next contact */
    if (g_on_select) g_on_select(contact_idx);
}

static void row_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    ui_sidebar_open((int)(intptr_t)lv_obj_get_user_data(row));
}

/* One Tab = the next contact. Tab from the text box moves the group's focus
 * onto some row (whichever follows the text box in the group); whatever
 * row that is, open the contact after the one on screen, wrapping. With a
 * single contact there is nobody else: move on, back to the text box.
 *
 * The switch runs on the next tick, and only if a row still has focus
 * then: deleting a focused text box (a panel rebuild or eviction) also
 * moves the focus onto a row, in the middle of the delete, and the code
 * doing that delete focuses the new text box right after. Only for keys:
 * a tap focuses the row too, and its click opens that row. */

static void next_contact_async(void *unused)
{
    (void)unused;
    g_next_pending = false;
    if (!g_chat_active || !g_on_select || g_count == 0) return;
    lv_group_t *grp = lv_group_get_default();
    lv_obj_t *f = grp ? lv_group_get_focused(grp) : NULL;
    bool on_row = false;
    for (int i = 0; i < g_count; i++) on_row |= (f == g_rows[i]);
    if (!on_row) return;
    int next = g_selected >= 0 ? (g_selected + 1) % g_count : 0;
    if (next == g_selected) {
        lv_group_focus_next(grp);
        return;
    }
    g_on_select(next);
}

static void row_focus_cb(lv_event_t *e)
{
    (void)e;
    if (!g_chat_active || !g_on_select || g_count == 0 || g_next_pending) return;
    lv_indev_t *act = lv_indev_active();
    if (act && lv_indev_get_type(act) == LV_INDEV_TYPE_POINTER) return;
    g_next_pending = true;
    lv_async_call(next_contact_async, NULL);
}

void ui_sidebar_remove_from_group(void)
{
    for (int i = 0; i < g_count; i++)
        lv_group_remove_obj(g_rows[i]);
}

void ui_sidebar_add_to_group(lv_group_t *grp)
{
    for (int i = 0; i < g_count; i++)
        lv_group_add_obj(grp, g_rows[i]);
}

#ifdef CHATBOX_TESTING
lv_obj_t *ui_sidebar_get_row(int idx)
{
    if (idx < 0 || idx >= g_count) return NULL;
    return g_rows[idx];
}

lv_obj_t *ui_sidebar_get_dot(int idx)
{
    if (idx < 0 || idx >= g_count) return NULL;
    return g_dots[idx];
}

int ui_sidebar_get_count(void)
{
    return g_count;
}
#endif
