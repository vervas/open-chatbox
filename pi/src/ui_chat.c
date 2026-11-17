/**
 * ui_chat.c — Chat message screen implementation
 *
 * Builds inside a content panel (not full screen). Uses flex column layout:
 * header (UI_HEAD_H) | message area (flex grow) | input bar (112px).
 *
 * Look (ui_theme.h): the page colour behind everything; the contact's face
 * and name in chat blue on the left of the header (music peeks in at the
 * right, app_chat.c). Incoming bubbles are white; an outgoing bubble is
 * pale blue while it's on its way, blue once it arrived and red when it is
 * late (no words: the colour is the message). The text box carries the
 * focus look; Send is a blue sticker.
 *
 * Photo bubbles (m.image, docs/photos.md): the picture itself, rounded,
 * fitted in PHOTO_BUBBLE_W × PHOTO_BUBBLE_H with its shape kept. Until it
 * is downloaded (photo_inbox.c saves it into the photo folder) the bubble
 * is a white box of the photo's shape with a picture glyph; if it never
 * arrives (refused, failed) the glyph stays with a short note. Pictures
 * are decoded at bubble size (photo_decode.h) outside LVGL's heap, one per
 * tick, and only up to PHOTO_BUBBLE_BUDGET bytes in all. What gets a
 * picture first: the open chat's bubbles on screen, then its others newest
 * first, then the other cached chats' newest first (photo_rank). When the
 * budget is spent, a bubble that ranks higher takes the picture of the one
 * that ranks lowest (it shows the glyph again, at the same size, until it
 * ranks high enough: scrolled to, or its chat opened). Deleting the photo
 * in Photos doesn't touch a bubble that shows it; a bubble built after that
 * (or one that gave its picture back) shows the glyph.
 */

#include "ui_chat.h"
#include "ui_sidebar.h"
#include "emoji_font.h"
#include "prof.h"
#include "text_nfc.h"
#include "ui_theme.h"
#include "ui_photo_img.h"
#include "photo_inbox.h"
#include "src/others/gridnav/lv_gridnav.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Colors (ui_theme.h) */
#define CHAT_OUTGOING_COLOR UI_C_CHAT
#define CHAT_PENDING_COLOR  UI_C_CHAT_PALE   /* outgoing bubble, on its way */
#define CHAT_FAILED_COLOR   UI_C_ALERT       /* outgoing bubble, undelivered */

/* A bubble is marked red after this long without confirmed delivery. Red
 * means "not delivered yet after 60s", not "failed forever": the outbox now
 * retries transient failures indefinitely, so a red bubble turns back to the
 * normal outgoing colour if the send is eventually delivered (see
 * ui_chat_pending_complete). Only a permanent rejection leaves it red. */
#define PENDING_RED_AFTER_MS  60000u
#define PENDING_TICK_MS        1000u
#define PENDING_CAP             32

#define ROOM_ID_MAX 256

static void photo_timer_kick(void);
static void msg_scroll_end_cb(lv_event_t *e);

/* Bubble spacing */
#define BUBBLE_RADIUS       26
#define BUBBLE_PAD_HOR      20
#define BUBBLE_PAD_VER      12   /* emoji sit taller than text: keep them off the edge */
#define BUBBLE_GAP           6   /* between messages from the same side */
#define BUBBLE_SENDER_GAP   16   /* when the sender changes */
#define PHOTO_BUBBLE_W     360   /* a photo bubble fits in this box */
#define PHOTO_BUBBLE_H     300
#define PHOTO_BUBBLE_BUDGET (8u * 1024u * 1024u)   /* decoded bubble pixels, all panels */

/* Custom control codes mapped in SDL and XKB drivers (see patches/) */
#define LV_KEY_MENU    0x04   /* Menu / Windows key — toggle emoji panel */
#define LV_KEY_PG_UP   0x05   /* Page Up — previous emoji page */
#define LV_KEY_PG_DOWN 0x06   /* Page Down — next emoji page */

/* Emoji sheet buttons: a page is 35 emoji + the ×, 12 across × 3 rows in
 * the sheet's 340 px (wider buttons wrap to a 4th row the sheet scrolls
 * to). The 48 px emoji PNGs are drawn at their own size. */
#define EMOJI_BTN      84
/* Space between the sheet and the screen's right edge: the same as the
 * gap between the contacts' shelf and the sheet on its left. */
#define EMOJI_SHEET_MARGIN 12

/* Emoji entry: UTF-8 string for textarea + asset basename for the PNG */
typedef struct {
    const char *utf8;
    const char *asset;
} emoji_entry_t;

/* A page of emojis in the picker */
typedef struct {
    const char         *title;
    const emoji_entry_t *entries;
    size_t              count;
} emoji_page_t;

#include "emoji_pages.inc"

/* Per-panel state. One instance per cached chat room, attached to the
 * root panel via lv_obj_set_user_data. The module globals below point at
 * whichever panel is currently active. */
typedef struct {
    lv_obj_t *root;          /* the panel itself (== parent passed to create) */
    lv_obj_t *msg_cont;
    lv_obj_t *textarea;
    lv_obj_t *send_btn;
    lv_obj_t *emoji_panel;
    lv_obj_t *emoji_header_label;
    lv_obj_t *emoji_close_btn;
    lv_obj_t *new_divider;
    lv_obj_t *head_avatar;   /* image in a round clip */
    lv_obj_t *head_initial;
    chat_lang_t lang;        /* accent combo language (ui_chat_set_language) */
    char      room_id[ROOM_ID_MAX];
} ui_chat_panel_state_t;

/* Module-level state — points at the currently-active panel's widgets. */
static lv_obj_t *g_parent       = NULL;
static lv_obj_t *g_msg_cont     = NULL;
static lv_obj_t *g_textarea     = NULL;
static lv_obj_t *g_send_btn     = NULL;
static lv_obj_t *g_emoji_panel        = NULL;
static lv_obj_t *g_emoji_header_label = NULL;
static lv_obj_t *g_emoji_close_btn    = NULL;
static lv_obj_t *g_new_divider        = NULL;

/* Persists across panel open/close so user returns to last viewed page. */
static size_t g_emoji_page_idx = 0;

static void (*g_on_send)(const char *, void *) = NULL;

static char g_room_id[ROOM_ID_MAX] = {0};

/* ---- Pending-send tracking -------------------------------------------- */

/* One slot per optimistic bubble awaiting delivery confirmation. The
 * `send_ctx` handed to the network callback is just the slot index + 1
 * cast through (void *), so 0/NULL means "not tracked". */
typedef struct {
    bool      in_use;
    lv_obj_t *bubble;       /* NULL once the bubble widget is destroyed */
    uint32_t  enqueued_ms;  /* lv_tick_get() snapshot for the 60s threshold */
    bool      red;          /* already recoloured; idempotency guard */
} pending_send_t;

static pending_send_t g_pending[PENDING_CAP];
static lv_timer_t    *g_pending_timer = NULL;

static void *pending_ctx_from_slot(int slot)
{
    return (void *)(intptr_t)(slot + 1);
}

static int pending_slot_from_ctx(void *ctx)
{
    intptr_t v = (intptr_t)ctx;
    if (v <= 0 || v > PENDING_CAP) return -1;
    return (int)(v - 1);
}

/* An outgoing bubble's colour says where it is: pale while on its way,
 * blue once delivered, red when late. The label inherits the text colour. */
static void bubble_paint(lv_obj_t *bubble, lv_color_t bg, lv_color_t ink)
{
    if (!bubble) return;
    lv_obj_set_style_bg_color(bubble, bg, 0);
    lv_obj_set_style_text_color(bubble, ink, 0);
}

/* Recolour a bubble to the "undelivered" style. Used both by the pending
 * tracker (via slot) and directly in do_send when the tracker is full. */
static void bubble_mark_failed(lv_obj_t *bubble)
{
    bubble_paint(bubble, CHAT_FAILED_COLOR, UI_C_ON_ALERT);
}

static void pending_mark_red(int slot)
{
    if (slot < 0 || slot >= PENDING_CAP) return;
    pending_send_t *p = &g_pending[slot];
    if (!p->in_use || p->red) return;
    bubble_mark_failed(p->bubble);
    p->red = true;
}

/* Delivered: the bubble turns the normal outgoing blue, from pale (on its
 * way) or from red (a late delivery: it reads as a normal message again
 * rather than a permanent failure). Guarded the same way as
 * pending_mark_red — p->bubble is nulled by pending_bubble_deleted_cb when
 * the widget is destroyed. */
static void pending_delivered(int slot)
{
    if (slot < 0 || slot >= PENDING_CAP) return;
    pending_send_t *p = &g_pending[slot];
    if (!p->in_use) return;
    bubble_paint(p->bubble, CHAT_OUTGOING_COLOR, UI_C_ON_CHAT);
    p->red = false;
}

static void pending_release(int slot)
{
    if (slot < 0 || slot >= PENDING_CAP) return;
    memset(&g_pending[slot], 0, sizeof(g_pending[slot]));
}

static void pending_bubble_deleted_cb(lv_event_t *e)
{
    int slot = pending_slot_from_ctx(lv_event_get_user_data(e));
    if (slot < 0) return;
    /* Bubble widget gone (panel eviction etc.); keep the slot reserved
     * until the network callback releases it, but null the pointer so
     * pending_tick / pending_mark_red won't touch freed memory. */
    g_pending[slot].bubble = NULL;
}

static void pending_tick_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < PENDING_CAP; i++) {
        pending_send_t *p = &g_pending[i];
        if (!p->in_use || p->red) continue;
        if (lv_tick_elaps(p->enqueued_ms) >= PENDING_RED_AFTER_MS) {
            pending_mark_red(i);
        }
    }
}

static int pending_register(lv_obj_t *bubble)
{
    if (!g_pending_timer) {
        g_pending_timer = lv_timer_create(pending_tick_cb, PENDING_TICK_MS, NULL);
    }
    for (int i = 0; i < PENDING_CAP; i++) {
        if (g_pending[i].in_use) continue;
        g_pending[i].in_use      = true;
        g_pending[i].bubble      = bubble;
        g_pending[i].enqueued_ms = lv_tick_get();
        g_pending[i].red         = false;
        lv_obj_add_event_cb(bubble, pending_bubble_deleted_cb,
                            LV_EVENT_DELETE, pending_ctx_from_slot(i));
        return i;
    }
    /* Outbox holds 32 sends; pending mirrors that. If we ever run out,
     * the caller (do_send) is responsible for marking the bubble failed
     * immediately: an untracked bubble is invisible to pending_tick_cb,
     * so it would otherwise stay the normal outgoing colour forever even
     * though the send can't be tracked (and the same-depth outbox would
     * reject it). */
    return -1;
}

void ui_chat_pending_complete(void *send_ctx, bool failed)
{
    int slot = pending_slot_from_ctx(send_ctx);
    if (slot < 0) return;
    if (failed) pending_mark_red(slot);
    else        pending_delivered(slot);   /* blue, also after a late (red) delivery */
    pending_release(slot);
}

static ui_chat_panel_state_t *get_state(lv_obj_t *panel)
{
    if (!panel) return NULL;
    return (ui_chat_panel_state_t *)lv_obj_get_user_data(panel);
}

static void sync_state_from_globals(ui_chat_panel_state_t *st)
{
    if (!st) return;
    st->msg_cont           = g_msg_cont;
    st->textarea           = g_textarea;
    st->send_btn           = g_send_btn;
    st->emoji_panel        = g_emoji_panel;
    st->emoji_header_label = g_emoji_header_label;
    st->emoji_close_btn    = g_emoji_close_btn;
    st->new_divider        = g_new_divider;
}

static void load_globals_from_state(ui_chat_panel_state_t *st)
{
    g_parent             = st ? st->root : NULL;
    g_msg_cont           = st ? st->msg_cont : NULL;
    g_textarea           = st ? st->textarea : NULL;
    g_send_btn           = st ? st->send_btn : NULL;
    g_emoji_panel        = st ? st->emoji_panel : NULL;
    g_emoji_header_label = st ? st->emoji_header_label : NULL;
    g_emoji_close_btn    = st ? st->emoji_close_btn : NULL;
    g_new_divider        = st ? st->new_divider : NULL;
    photo_timer_kick();     /* another chat's photo bubbles come first now */
    if (st) {
        strncpy(g_room_id, st->room_id, ROOM_ID_MAX - 1);
        g_room_id[ROOM_ID_MAX - 1] = '\0';
    } else {
        g_room_id[0] = '\0';
    }
}

/* Defer textarea focus past the current event chain.
 * lv_group_focus_obj() called from inside an LV_EVENT_FOCUSED handler
 * (which is how show_chat is reached when Tab moves the sidebar focus)
 * gets fragile — LVGL's group machinery doesn't always honor a
 * re-entrant focus move. lv_async_call schedules the focus on the
 * next lv_timer_handler tick, by which time the focus chain has
 * unwound and the move sticks. */
static void focus_textarea_async(void *user_data)
{
    lv_obj_t *ta = (lv_obj_t *)user_data;
    if (ta) lv_group_focus_obj(ta);
}

/* Forward declarations */
static void send_btn_cb(lv_event_t *e);
static void textarea_event_cb(lv_event_t *e);
static void textarea_insert_cb(lv_event_t *e);
static void emoji_toggle_cb(lv_event_t *e);
static void emoji_close_cb(lv_event_t *e);
static void emoji_click_cb(lv_event_t *e);
static void chat_key_cb(lv_event_t *e);
static void create_emoji_panel(void);
static void destroy_emoji_panel(void);
static void populate_emoji_panel(size_t page_idx);
static void emoji_change_page(int delta);

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void ui_chat_create(lv_obj_t *parent, const char *contact_name,
                    const char *room_id)
{
    /* Allocate per-panel state and stash on the panel */
    ui_chat_panel_state_t *st = lv_malloc(sizeof(*st));
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->root = parent;
    strncpy(st->room_id, room_id ? room_id : "", ROOM_ID_MAX - 1);
    st->room_id[ROOM_ID_MAX - 1] = '\0';
    lv_obj_set_user_data(parent, st);

    /* Make this panel the active one — module globals point at its widgets */
    load_globals_from_state(st);
    g_on_send  = NULL;

    /* Parent setup: flex column fills available space */
    lv_obj_set_style_bg_color(parent, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_radius(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Header: the contact's face and name, chat blue -------------------- */
    lv_obj_t *header = ui_obj(parent);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, UI_HEAD_H);
    lv_obj_set_style_pad_left(header, 20, 0);
    lv_obj_set_style_pad_column(header, 14, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *face = ui_obj(header);
    lv_obj_set_size(face, 48, 48);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(face, true, 0);
    lv_obj_set_style_bg_color(face, UI_C_CHAT_SOFT, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    st->head_avatar = lv_image_create(face);
    lv_obj_set_size(st->head_avatar, 48, 48);
    lv_image_set_inner_align(st->head_avatar, LV_IMAGE_ALIGN_STRETCH);
    st->head_initial = ui_label(face, &font_title_28, UI_C_CHAT_INK);
    lv_obj_center(st->head_initial);

    lv_obj_t *name_lbl = ui_label(header, &font_title_36, UI_C_CHAT_INK);
    lv_label_set_text(name_lbl, contact_name ? contact_name : "");

    /* ---- Message area (grows to fill space) ----------------------------- */
    g_msg_cont = lv_obj_create(parent);
    lv_obj_set_width(g_msg_cont, lv_pct(100));
    lv_obj_set_flex_grow(g_msg_cont, 1);
    lv_obj_set_flex_flow(g_msg_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_msg_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(g_msg_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_msg_cont, 0, 0);
    lv_obj_set_style_radius(g_msg_cont, 0, 0);
    lv_obj_set_style_pad_hor(g_msg_cont, 16, 0);
    lv_obj_set_style_pad_ver(g_msg_cont, 12, 0);
    lv_obj_set_style_pad_gap(g_msg_cont, BUBBLE_GAP, 0);
    /* Scrolled: photo bubbles that came into view may get their picture. */
    lv_obj_add_event_cb(g_msg_cont, msg_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    /* Scrollbar hugs the screen edge, clear of the outgoing bubbles */
    lv_obj_set_style_pad_right(g_msg_cont, 2, LV_PART_SCROLLBAR);

    /* ---- Input bar (112px tall at bottom) ------------------------------- */
    lv_obj_t *input_bar = lv_obj_create(parent);
    lv_obj_set_width(input_bar, lv_pct(100));
    /* 80 px controls with room around them for the focus ring and the
     * stickers' solid shadows */
    lv_obj_set_height(input_bar, 112);
    lv_obj_set_style_bg_opa(input_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_bar, 0, 0);
    lv_obj_set_style_radius(input_bar, 0, 0);
    lv_obj_set_style_pad_top(input_bar, 8, 0);
    lv_obj_set_style_pad_bottom(input_bar, 24, 0);
    lv_obj_set_style_pad_left(input_bar, 8, 0);   /* the emoji button is transparent */
    lv_obj_set_style_pad_right(input_bar, 28, 0);
    lv_obj_set_style_pad_gap(input_bar, 16, 0);
    lv_obj_add_flag(input_bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Smiley toggle button (opens/closes emoji picker) — touch/click only */
    lv_obj_t *emoji_btn = lv_btn_create(input_bar);
    lv_obj_set_size(emoji_btn, 80, 80);
    lv_obj_set_style_bg_opa(emoji_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(emoji_btn, 0, 0);
    lv_obj_set_style_radius(emoji_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(emoji_btn, 8, 0);
    lv_obj_add_event_cb(emoji_btn, emoji_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_group_remove_obj(emoji_btn);  /* not in Tab cycle */

    char emoji_btn_src[256];
    snprintf(emoji_btn_src, sizeof(emoji_btn_src), "A:%s/%04x.png",
             emoji_font_get_dir(), 0x1F600);
    lv_obj_t *emoji_img = lv_image_create(emoji_btn);
    lv_image_set_src(emoji_img, emoji_btn_src);
    lv_image_set_scale(emoji_img, 384);   /* 1.5x: 48px -> 72px */
    lv_obj_center(emoji_img);

    /* Textarea (grows to fill available space) */
    g_textarea = lv_textarea_create(input_bar);
    lv_obj_set_flex_grow(g_textarea, 1);
    lv_textarea_set_one_line(g_textarea, true);
    lv_obj_set_height(g_textarea, 80);
    char ph[96];
    snprintf(ph, sizeof(ph), "Write to %s\xE2\x80\xA6", contact_name ? contact_name : "");
    lv_textarea_set_placeholder_text(g_textarea, ph);
    lv_obj_set_style_bg_color(g_textarea, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(g_textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_textarea, UI_RADIUS, 0);
    lv_obj_set_style_text_color(g_textarea, UI_C_INK, 0);
    lv_obj_set_style_text_color(g_textarea, UI_C_INK_2, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(g_textarea, UI_C_CHAT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(g_textarea, 3, LV_PART_CURSOR);
    lv_obj_set_style_pad_hor(g_textarea, 20, 0);
    ui_item_focus_states(g_textarea, UI_APP_CHAT, true);
    const lv_font_t *ta_font = emoji_font_get_36();
    if (!ta_font) ta_font = &font_chat_48;   /* the emoji font's own fallback */
    lv_obj_set_style_text_font(g_textarea, ta_font, 0);
    /* Fit one line exactly: the theme's padding leaves less than the font's
     * 60 px line, and the textarea then scrolls a few px as you type */
    int32_t ta_pad = (80 - lv_font_get_line_height(ta_font)
                      - 2 * lv_obj_get_style_border_width(g_textarea, 0)) / 2;
    lv_obj_set_style_pad_ver(g_textarea, ta_pad > 0 ? ta_pad : 0, 0);
    lv_obj_add_event_cb(g_textarea, textarea_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_textarea, textarea_insert_cb, LV_EVENT_INSERT, st);

    /* Send button: a blue sticker (not focusable: Enter sends) */
    g_send_btn = lv_btn_create(input_bar);
    lv_obj_set_size(g_send_btn, 160, 80);
    lv_obj_set_style_bg_color(g_send_btn, UI_C_CHAT, 0);
    lv_obj_set_style_radius(g_send_btn, UI_RADIUS, 0);
    ui_sticker(g_send_btn, 4, 6);
    lv_obj_add_event_cb(g_send_btn, send_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_group_remove_obj(g_send_btn);  /* not in Tab cycle — Enter on textarea sends */

    lv_obj_t *send_lbl = lv_label_create(g_send_btn);
    lv_label_set_text(send_lbl, "Send");
    lv_obj_set_style_text_font(send_lbl, &font_title_36, 0);
    lv_obj_set_style_text_color(send_lbl, UI_C_ON_CHAT, 0);
    lv_obj_center(send_lbl);

    /* Register Menu/Esc key handler on textarea and send button */
    lv_obj_add_event_cb(g_textarea, chat_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(g_send_btn, chat_key_cb, LV_EVENT_KEY, NULL);

    /* Keyboard group: sidebar rows stay for Tab contact-switching.
     * Add textarea so it receives typed characters when focused.
     * Send button is NOT in the group (Enter on textarea sends). */
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, g_textarea);
        /* Synchronous focus is fine here: ui_chat_create is reached via
         * row_click_cb (LV_EVENT_CLICKED), which doesn't have the
         * re-entrant-FOCUSED-handler issue that ui_chat_attach faces. */
        lv_group_focus_obj(g_textarea);
    }

    /* Persist widget pointers onto the panel state so ui_chat_attach()
     * can restore them later. */
    sync_state_from_globals(st);
}

/* -------------------------------------------------------------------------
 * Multi-panel attach / detach (Step 5)
 * ---------------------------------------------------------------------- */

void ui_chat_attach(lv_obj_t *panel)
{
    ui_chat_panel_state_t *st = get_state(panel);
    if (!st) return;
    load_globals_from_state(st);
    if (g_textarea) lv_async_call(focus_textarea_async, g_textarea);
}

void ui_chat_detach(void)
{
    /* Cancel any pending deferred focus on the active textarea. In the
     * production hide-and-cache path this is a no-op (textarea stays
     * alive); in test paths that lv_obj_clean the panel after detach,
     * this prevents the async callback from firing on a freed pointer. */
    if (g_textarea) lv_async_call_cancel(focus_textarea_async, g_textarea);

    /* Snapshot any updated pointers (e.g. emoji_panel was opened/closed)
     * back into the active panel's state before clearing globals. */
    ui_chat_panel_state_t *st = get_state(g_parent);
    if (st) sync_state_from_globals(st);
    load_globals_from_state(NULL);
    g_on_send = NULL;
}

void ui_chat_free_panel_state(lv_obj_t *panel)
{
    ui_chat_panel_state_t *st = get_state(panel);
    if (!st) return;
    lv_obj_set_user_data(panel, NULL);
    lv_free(st);
}

const char *ui_chat_panel_room_id(lv_obj_t *panel)
{
    ui_chat_panel_state_t *st = get_state(panel);
    return st ? st->room_id : "";
}

/* Strip variation selectors U+FE0E / U+FE0F from UTF-8 text. The imgfont
 * has no PNG for them and the text fallback font has no glyph, so they
 * render as tofu boxes after every emoji in incoming messages. The
 * preceding base codepoint already triggers emoji presentation, so the
 * VS is purely cosmetic and safe to drop for display.
 *
 * Returns an lv_malloc'd copy; caller must lv_free. */
static char *strip_variation_selectors(const char *src)
{
    size_t n = strlen(src);
    char *out = lv_malloc(n + 1);
    if (!out) return NULL;
    char *w = out;
    for (size_t i = 0; i < n; ) {
        if (i + 3 <= n
            && (uint8_t)src[i]     == 0xEF
            && (uint8_t)src[i + 1] == 0xB8
            && ((uint8_t)src[i + 2] == 0x8E
                || (uint8_t)src[i + 2] == 0x8F)) {
            i += 3;
            continue;
        }
        *w++ = src[i++];
    }
    *w = '\0';
    return out;
}

/* A bubble's row (left/right alignment), with extra space above it when
 * the sender changes (rows remember their side in user_data: 1 =
 * incoming, 2 = outgoing). */
static lv_obj_t *new_row(lv_obj_t *msg_cont, bool is_from_me)
{
    uintptr_t side = is_from_me ? 2 : 1;
    lv_obj_t *prev = lv_obj_get_child(msg_cont, -1);
    uintptr_t prev_side = prev ? (uintptr_t)lv_obj_get_user_data(prev) : 0;

    lv_obj_t *row = lv_obj_create(msg_cont);
    lv_obj_set_user_data(row, (void *)side);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    if (prev_side && prev_side != side)
        lv_obj_set_style_pad_top(row, BUBBLE_SENDER_GAP - BUBBLE_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* Internal: add a bubble to an arbitrary message container.
 * Returns the bubble lv_obj_t so do_send can register it for pending-send
 * tracking; callers that don't need it can discard. */
static lv_obj_t *add_message_to_cont(lv_obj_t *msg_cont, const char *text, bool is_from_me, bool scroll)
{
    if (!msg_cont || !text) return NULL;
    PROF_START(t0);

    /* Use parent width for bubble sizing */
    int32_t cont_w = lv_obj_get_width(msg_cont);
    if (cont_w < 100) cont_w = 600;  /* fallback before layout */
    int32_t max_bubble_w = (cont_w * 75) / 100;

    lv_obj_t *row = new_row(msg_cont, is_from_me);

    /* Bubble */
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, max_bubble_w, 0);
    lv_obj_set_style_radius(bubble, BUBBLE_RADIUS, 0);
    lv_obj_set_style_pad_hor(bubble, BUBBLE_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(bubble, BUBBLE_PAD_VER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (is_from_me) {
        bubble_paint(bubble, CHAT_OUTGOING_COLOR, UI_C_ON_CHAT);
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, -8, 0);
    } else {
        bubble_paint(bubble, UI_C_SURFACE, UI_C_INK);
        lv_obj_set_style_border_width(bubble, 2, 0);
        lv_obj_set_style_border_color(bubble, UI_C_LINE, 0);
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 8, 0);
    }

    /* Message label — use emoji font if available, else its text fallback */
    const lv_font_t *msg_font = emoji_font_get();
    if (!msg_font) msg_font = &font_chat_36;

    lv_obj_t *lbl = lv_label_create(bubble);
    char *clean = strip_variation_selectors(text);
    if (clean) text_utf8_nfc(clean);
    lv_label_set_text(lbl, clean ? clean : text);
    if (clean) lv_free(clean);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, max_bubble_w - 2 * BUBBLE_PAD_HOR, 0);
    lv_obj_set_style_text_font(lbl, msg_font, 0);   /* colour: the bubble's */

    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* Auto-scroll to bottom only when adding to the visible/active panel */
    if (scroll) lv_obj_scroll_to_y(msg_cont, LV_COORD_MAX, LV_ANIM_ON);

    PROF_WARN_IF_OVER(t0, 50, "ui_chat_add_message len=%zu", strlen(text));
    return bubble;
}

void ui_chat_add_message(const char *text, bool is_from_me)
{
    add_message_to_cont(g_msg_cont, text, is_from_me, true);
}

void ui_chat_add_message_to(lv_obj_t *panel, const char *text, bool is_from_me)
{
    if (!panel) return;
    ui_chat_panel_state_t *st = get_state(panel);
    if (!st) return;
    bool scroll = (panel == g_parent);
    add_message_to_cont(st->msg_cont, text, is_from_me, scroll);
}

/* -------------------------------------------------------------------------
 * Photo bubbles
 * ---------------------------------------------------------------------- */

typedef struct photo_bubble {
    struct photo_bubble *prev, *next;   /* g_photos list, newest first */
    uint32_t       seq;                 /* add order: message order within a chat */
    lv_obj_t      *msg_cont;
    lv_obj_t      *box;                 /* the bubble */
    lv_obj_t      *img;
    lv_obj_t      *glyph;
    lv_obj_t      *note;
    ui_photo_img_t pic;
    char           event_id[256];
    int64_t        ts;
    bool           is_from_me;
    bool           want;                /* would show a picture if decoded */
} photo_bubble_t;

static photo_bubble_t *g_photos = NULL;
static size_t          g_photo_bytes = 0;
static lv_timer_t     *g_photo_timer = NULL;
static uint32_t        g_photo_seq = 0;

/* The most one bubble's picture can take. */
#define PHOTO_BUBBLE_MAX_BYTES ((size_t)PHOTO_BUBBLE_W * PHOTO_BUBBLE_H * 3)

static size_t photo_bytes(const photo_bubble_t *b)
{
    return b->pic.px.px ? (size_t)b->pic.px.w * (size_t)b->pic.px.h * 3 : 0;
}

static void photo_bubble_delete_cb(lv_event_t *e)
{
    photo_bubble_t *b = lv_event_get_user_data(e);
    if (!b) return;
    g_photo_bytes -= photo_bytes(b);
    ui_photo_img_free(&b->pic);
    if (b->prev) b->prev->next = b->next;
    else         g_photos = b->next;
    if (b->next) b->next->prev = b->prev;
    free(b);
    photo_timer_kick();       /* memory came back: a waiting bubble may fit */
}

/* Set the glyph / note look for a bubble without its picture. */
static void photo_bubble_placeholder(photo_bubble_t *b, photo_status_t st)
{
    const char *note = st == PHOTO_ST_FAILED  ? "Photo didn't arrive" :
                       st == PHOTO_ST_REFUSED ? "Photo can't be shown" : NULL;
    ui_photo_img_unset(b->img);
    lv_obj_add_flag(b->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(b->glyph, LV_OBJ_FLAG_HIDDEN);
    if (note) {
        lv_label_set_text(b->note, note);
        lv_obj_clear_flag(b->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(b->note, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_opa(b->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b->box, b->is_from_me ? 0 : 2, 0);
}

/* Look up the bubble's photo: show the glyph, or mark it to be decoded. */
static void photo_bubble_refresh(photo_bubble_t *b)
{
    if (ui_photo_img_ok(&b->pic)) return;       /* showing: stays as it is */
    photo_status_t st = photo_inbox_status(b->event_id, b->ts, NULL, 0);
    b->want = st == PHOTO_ST_READY;
    photo_bubble_placeholder(b, st);
}

/* The bubble is (at least partly) inside its message list's view. */
static bool photo_on_screen(const photo_bubble_t *b)
{
    lv_area_t a, v;
    lv_obj_get_coords(b->box, &a);
    lv_obj_get_coords(b->msg_cont, &v);
    return a.y2 >= v.y1 && a.y1 <= v.y2;
}

/* How much a bubble's picture matters (higher first): the open chat's
 * bubbles on screen, then the open chat's others, then other chats';
 * within each, the newest. */
static uint64_t photo_rank(const photo_bubble_t *b)
{
    uint64_t r = b->seq;
    if (b->msg_cont == g_msg_cont) {
        r |= 1ull << 32;
        if (photo_on_screen(b)) r |= 1ull << 33;
    }
    return r;
}

/* The decoded bubble that matters least, if it matters less than `rank`. */
static photo_bubble_t *photo_victim(uint64_t rank)
{
    photo_bubble_t *v = NULL;
    uint64_t vr = 0;
    for (photo_bubble_t *b = g_photos; b; b = b->next) {
        if (!ui_photo_img_ok(&b->pic)) continue;
        uint64_t r = photo_rank(b);
        if (r < rank && (!v || r < vr)) { v = b; vr = r; }
    }
    return v;
}

/* Give b's picture back: the glyph again, at the same size (nothing
 * moves); it is decoded again when it ranks high enough. */
static void photo_bubble_unload(photo_bubble_t *b)
{
    g_photo_bytes -= photo_bytes(b);
    ui_photo_img_unset(b->img);
    ui_photo_img_free(&b->pic);
    photo_bubble_placeholder(b, PHOTO_ST_READY);
    b->want = true;
}

/* Make room for one more picture for a bubble of `rank`: false when the
 * budget is spent on pictures that all matter more. */
static bool photo_make_room(uint64_t rank)
{
    while (g_photo_bytes + PHOTO_BUBBLE_MAX_BYTES > PHOTO_BUBBLE_BUDGET) {
        photo_bubble_t *v = photo_victim(rank);
        if (!v) return false;
        photo_bubble_unload(v);
    }
    return true;
}

/* Decode b's picture now (it is READY), making room for it first. */
static void photo_bubble_load(photo_bubble_t *b)
{
    if (!photo_make_room(photo_rank(b))) return;
    b->want = false;
    char path[1024];
    if (photo_inbox_status(b->event_id, b->ts, path, sizeof(path)) != PHOTO_ST_READY) {
        photo_bubble_refresh(b);
        return;
    }
    if (!ui_photo_img_load(&b->pic, path, PHOTO_BUBBLE_W, PHOTO_BUBBLE_H, PHOTO_FIT_CONTAIN)) {
        photo_bubble_placeholder(b, PHOTO_ST_REFUSED);
        return;
    }
    g_photo_bytes += photo_bytes(b);
    /* Pinned to the bottom: there, or on the way there. The only animation
     * on a message list is our scroll to the bottom (a new bubble), and its
     * end was fixed when it started: a box that changes size now (the
     * event gave no size, or another one) would leave it short. */
    bool sliding = lv_anim_get(b->msg_cont, NULL) != NULL;
    bool at_bottom = sliding || lv_obj_get_scroll_bottom(b->msg_cont) < 40;
    lv_obj_set_size(b->box, b->pic.px.w, b->pic.px.h);
    lv_image_set_src(b->img, &b->pic.dsc);
    lv_obj_clear_flag(b->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(b->glyph, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(b->note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(b->box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b->box, 0, 0);
    if (at_bottom) {
        lv_obj_update_layout(b->msg_cont);
        lv_obj_scroll_to_y(b->msg_cont, LV_COORD_MAX, sliding ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

/* The next bubble to decode: the waiting one that ranks highest, if the
 * budget has room for it or holds a picture that ranks lower. */
static photo_bubble_t *photo_next(void)
{
    if (g_msg_cont) lv_obj_update_layout(g_msg_cont);    /* on-screen needs coords */
    photo_bubble_t *best = NULL;
    uint64_t br = 0;
    for (photo_bubble_t *b = g_photos; b; b = b->next) {
        if (!b->want) continue;
        uint64_t r = photo_rank(b);
        if (!best || r > br) { best = b; br = r; }
    }
    if (!best) return NULL;
    if (g_photo_bytes + PHOTO_BUBBLE_MAX_BYTES <= PHOTO_BUBBLE_BUDGET) return best;
    return photo_victim(br) ? best : NULL;
}

/* The loader: one picture per tick; it pauses itself when nothing waits. */
static void photo_timer_cb(lv_timer_t *t)
{
    photo_bubble_t *b = photo_next();
    if (b) photo_bubble_load(b);
    else lv_timer_pause(t);
}

/* What gets a picture may have changed (a bubble added or deleted, a
 * scroll, another chat opened): the loader looks again on its next tick.
 * Only resumes the timer, so it is safe from event and delete callbacks;
 * the timer is made by the first photo bubble. */
static void photo_timer_kick(void)
{
    if (g_photo_timer) lv_timer_resume(g_photo_timer);
}

static void msg_scroll_end_cb(lv_event_t *e)
{
    (void)e;
    photo_timer_kick();
}

static void add_photo_to_cont(lv_obj_t *msg_cont, const char *event_id, int64_t ts,
                              int info_w, int info_h, bool is_from_me, bool scroll)
{
    if (!msg_cont || !event_id) return;
    photo_bubble_t *b = calloc(1, sizeof(*b));
    if (!b) return;
    b->seq = ++g_photo_seq;
    snprintf(b->event_id, sizeof(b->event_id), "%s", event_id);
    b->ts = ts;
    b->is_from_me = is_from_me;
    b->msg_cont = msg_cont;

    lv_obj_t *row = new_row(msg_cont, is_from_me);
    /* The box has the photo's shape when the event says it (no jump when
     * the picture arrives), else 4:3. */
    int w = PHOTO_BUBBLE_W, h = PHOTO_BUBBLE_W * 3 / 4;
    if (info_w > 0 && info_h > 0)
        photo_fit_size(info_w, info_h, PHOTO_BUBBLE_W, PHOTO_BUBBLE_H, PHOTO_FIT_CONTAIN, &w, &h);
    if (w < 160) w = 160;          /* room for the glyph and a note */
    if (h < 120) h = 120;
    lv_obj_t *box = ui_obj(row);
    b->box = box;
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, BUBBLE_RADIUS, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_bg_color(box, is_from_me ? UI_C_CHAT_SOFT : UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, UI_C_LINE, 0);
    lv_obj_set_style_border_width(box, is_from_me ? 0 : 2, 0);
    if (is_from_me) lv_obj_align(box, LV_ALIGN_RIGHT_MID, -8, 0);
    else            lv_obj_align(box, LV_ALIGN_LEFT_MID, 8, 0);

    b->img = lv_image_create(box);
    lv_obj_set_pos(b->img, 0, 0);
    lv_obj_add_flag(b->img, LV_OBJ_FLAG_HIDDEN);
    b->glyph = ui_label(box, &lv_font_montserrat_48, UI_C_INK_2);
    lv_label_set_text(b->glyph, LV_SYMBOL_IMAGE);
    lv_obj_align(b->glyph, LV_ALIGN_CENTER, 0, -14);
    b->note = ui_label(box, &font_chat_28, UI_C_INK_2);
    lv_obj_align(b->note, LV_ALIGN_CENTER, 0, 34);
    lv_obj_add_flag(b->note, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(box, photo_bubble_delete_cb, LV_EVENT_DELETE, b);
    if (!g_photo_timer) g_photo_timer = lv_timer_create(photo_timer_cb, 16, NULL);
    b->next = g_photos;
    if (g_photos) g_photos->prev = b;
    g_photos = b;

    photo_bubble_refresh(b);
    photo_timer_kick();
    if (scroll) lv_obj_scroll_to_y(msg_cont, LV_COORD_MAX, LV_ANIM_ON);
}

void ui_chat_add_photo(const char *event_id, int64_t ts, int w, int h, bool is_from_me)
{
    add_photo_to_cont(g_msg_cont, event_id, ts, w, h, is_from_me, true);
}

void ui_chat_add_photo_to(lv_obj_t *panel, const char *event_id, int64_t ts, int w, int h,
                          bool is_from_me)
{
    ui_chat_panel_state_t *st = get_state(panel);
    if (!st) return;
    add_photo_to_cont(st->msg_cont, event_id, ts, w, h, is_from_me, panel == g_parent);
}

void ui_chat_photo_changed(const char *event_id)
{
    if (!event_id) return;
    for (photo_bubble_t *b = g_photos; b; b = b->next)
        if (strcmp(b->event_id, event_id) == 0) photo_bubble_refresh(b);
    photo_timer_kick();
}

void ui_chat_set_send_callback(void (*on_send)(const char *text, void *send_ctx))
{
    g_on_send = on_send;
}

void ui_chat_destroy(void)
{
    /* Equivalent to detach — kept for back-compat with older callers
     * that aren't aware of the multi-panel cache. Caller is expected
     * to lv_obj_clean() the parent if it wants the widgets gone. */
    ui_chat_detach();
}

const char *ui_chat_get_room_id(void)
{
    return g_room_id;
}

void ui_chat_insert_new_divider(void)
{
    if (g_new_divider || !g_msg_cont) return;

    /* Full-width row with a centered "New" label between two lines */
    g_new_divider = lv_obj_create(g_msg_cont);
    lv_obj_set_width(g_new_divider, lv_pct(100));
    lv_obj_set_height(g_new_divider, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(g_new_divider, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_new_divider, 0, 0);
    lv_obj_set_style_pad_all(g_new_divider, 4, 0);
    lv_obj_set_style_pad_gap(g_new_divider, 8, 0);
    lv_obj_set_flex_flow(g_new_divider, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_new_divider, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_new_divider, LV_OBJ_FLAG_SCROLLABLE);

    /* Left line */
    lv_obj_t *line_l = lv_obj_create(g_new_divider);
    lv_obj_set_flex_grow(line_l, 1);
    lv_obj_set_height(line_l, 2);
    lv_obj_set_style_bg_color(line_l, UI_C_ALERT, 0);
    lv_obj_set_style_bg_opa(line_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_l, 0, 0);
    lv_obj_set_style_radius(line_l, 1, 0);
    lv_obj_clear_flag(line_l, LV_OBJ_FLAG_SCROLLABLE);

    /* "New" label */
    lv_obj_t *lbl = lv_label_create(g_new_divider);
    lv_label_set_text(lbl, "New");
    lv_obj_set_style_text_font(lbl, &font_title_28, 0);
    lv_obj_set_style_text_color(lbl, UI_C_ALERT, 0);

    /* Right line */
    lv_obj_t *line_r = lv_obj_create(g_new_divider);
    lv_obj_set_flex_grow(line_r, 1);
    lv_obj_set_height(line_r, 2);
    lv_obj_set_style_bg_color(line_r, UI_C_ALERT, 0);
    lv_obj_set_style_bg_opa(line_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_r, 0, 0);
    lv_obj_set_style_radius(line_r, 1, 0);
    lv_obj_clear_flag(line_r, LV_OBJ_FLAG_SCROLLABLE);

    sync_state_from_globals(get_state(g_parent));
}

void ui_chat_clear_new_divider(void)
{
    if (!g_new_divider) return;
    lv_obj_delete(g_new_divider);
    g_new_divider = NULL;
    sync_state_from_globals(get_state(g_parent));
}

/* -------------------------------------------------------------------------
 * Private helpers
 * ---------------------------------------------------------------------- */

static void do_send(void)
{
    if (!g_textarea) return;
    const char *text = lv_textarea_get_text(g_textarea);
    if (!text || text[0] == '\0') return;

    /* Close emoji panel if open */
    if (g_emoji_panel) destroy_emoji_panel();

    /* Optimistic: show outgoing bubble immediately and track it so the
     * send callback can mark it red if delivery doesn't happen. */
    lv_obj_t *bubble = add_message_to_cont(g_msg_cont, text, true, true);
    bubble_paint(bubble, CHAT_PENDING_COLOR, UI_C_INK);   /* on its way */
    int slot = bubble ? pending_register(bubble) : -1;

    if (slot < 0) {
        /* Pending tracker is full (all 32 slots held during an outage). We
         * can't track this send's completion, and the same-depth outbox
         * would reject it with MATRIX_ERR_QUEUE anyway. Mark the bubble
         * failed now so the drop is never shown as delivered, and don't
         * attempt the send — an untracked send whose completion we can't
         * observe would falsely leave the bubble red even if it landed. */
        fprintf(stderr, "ui_chat: pending tracker full, marking send failed immediately\n");
        bubble_mark_failed(bubble);
        lv_textarea_set_text(g_textarea, "");
        return;
    }

    void *ctx = pending_ctx_from_slot(slot);

    if (g_on_send) {
        g_on_send(text, ctx);
    }

    lv_textarea_set_text(g_textarea, "");
}

static void send_btn_cb(lv_event_t *e)
{
    (void)e;
    do_send();
}

static void textarea_event_cb(lv_event_t *e)
{
    (void)e;
    do_send();
}

/* -------------------------------------------------------------------------
 * Emoji picker
 * ---------------------------------------------------------------------- */

/* Focus look for the emoji sheet's items: ring and shadow, but no lift.
 * gridnav picks the next item from the children's coordinates, and the
 * lift (a translate) moves the focused one up-left: from the lifted × the
 * emoji beside it scores as close as the one below, and Down went sideways. */
static void emoji_item_focus_states(lv_obj_t *o, bool idle_bg)
{
    ui_item_focus_states(o, UI_APP_CHAT, idle_bg);
    lv_obj_set_style_translate_x(o, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_translate_y(o, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_translate_x(o, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_translate_y(o, 0, LV_STATE_FOCUS_KEY);
}

static void create_emoji_panel(void)
{
    if (!g_parent) return;

    /* Create panel between message area and input bar.
     * Insert it as child index 2 (after header and msg_cont, before input_bar). */
    g_emoji_panel = lv_obj_create(g_parent);
    lv_obj_move_to_index(g_emoji_panel, 2);

    lv_obj_update_layout(g_parent);
    lv_obj_set_width(g_emoji_panel,
                     lv_obj_get_content_width(g_parent) - EMOJI_SHEET_MARGIN);
    lv_obj_set_height(g_emoji_panel, 340);
    lv_obj_set_style_bg_color(g_emoji_panel, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(g_emoji_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_emoji_panel, 0, 0);
    lv_obj_set_style_outline_width(g_emoji_panel, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(g_emoji_panel, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_radius(g_emoji_panel, 28, 0);
    lv_obj_set_style_pad_all(g_emoji_panel, 12, 0);
    lv_obj_set_style_pad_gap(g_emoji_panel, 6, 0);
    lv_obj_set_scrollbar_mode(g_emoji_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(g_emoji_panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g_emoji_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Header row — full width, takes its own row in the wrap layout: the
     * page's name on the left and a dot per page pinned to the right, so the
     * dots stay put as the names change. Non-focusable so gridnav skips it. */
    lv_obj_t *head = ui_obj(g_emoji_panel);
    lv_obj_set_size(head, lv_pct(100), 34);
    lv_obj_set_style_pad_hor(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g_emoji_header_label = ui_label(head, &font_title_28, UI_C_CHAT_INK);
    lv_obj_t *dots = ui_obj(head);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(dots, 8, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    for (size_t i = 0; i < EMOJI_PAGE_COUNT; i++) {
        lv_obj_t *d = ui_obj(dots);
        lv_obj_set_size(d, 14, 14);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, UI_C_SUNK, 0);
    }

    /* Close [X] button */
    g_emoji_close_btn = lv_btn_create(g_emoji_panel);
    lv_obj_set_size(g_emoji_close_btn, EMOJI_BTN, EMOJI_BTN);
    lv_obj_set_style_bg_color(g_emoji_close_btn, UI_C_SUNK, 0);
    lv_obj_set_style_radius(g_emoji_close_btn, 18, 0);
    emoji_item_focus_states(g_emoji_close_btn, true);
    lv_obj_add_event_cb(g_emoji_close_btn, emoji_close_cb, LV_EVENT_CLICKED, NULL);
    lv_group_remove_obj(g_emoji_close_btn);

    lv_obj_t *close_lbl = lv_label_create(g_emoji_close_btn);
    lv_label_set_text(close_lbl, "\xC3\x97");   /* × */
    lv_obj_set_style_text_font(close_lbl, &font_title_48, 0);
    lv_obj_set_style_text_color(close_lbl, UI_C_INK, 0);
    lv_obj_center(close_lbl);

    /* Register Menu/Esc/PgUp/PgDn key handler BEFORE gridnav so we fire first */
    lv_obj_add_event_cb(g_emoji_panel, chat_key_cb, LV_EVENT_KEY, NULL);

    /* Enable grid navigation on the panel — arrow keys move between children */
    lv_gridnav_add(g_emoji_panel, LV_GRIDNAV_CTRL_ROLLOVER);

    /* Group management: remove sidebar rows, keep textarea, add emoji panel.
     * Tab now bounces: emoji_panel ↔ textarea */
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        ui_sidebar_remove_from_group();
        lv_group_add_obj(grp, g_emoji_panel);
        lv_group_focus_obj(g_emoji_panel);
    }

    populate_emoji_panel(g_emoji_page_idx);

    /* Persist updated emoji widget pointers onto active panel state. */
    sync_state_from_globals(get_state(g_parent));
}

static void populate_emoji_panel(size_t page_idx)
{
    if (!g_emoji_panel || !g_emoji_header_label) return;
    if (page_idx >= EMOJI_PAGE_COUNT) page_idx = 0;

    PROF_START(t0);
    const emoji_page_t *page = &g_emoji_pages[page_idx];

    /* Header: the page's name, and its dot filled */
    lv_label_set_text(g_emoji_header_label, page->title);
    lv_obj_t *dots = lv_obj_get_sibling(g_emoji_header_label, 1);
    for (size_t i = 0; dots && i < EMOJI_PAGE_COUNT; i++)
        lv_obj_set_style_bg_color(lv_obj_get_child(dots, (int32_t)i),
                                  i == page_idx ? UI_C_CHAT : UI_C_SUNK, 0);

    /* Delete prior emoji buttons (header label + close button stay).
     * Iterate from the end so indices stay valid. */
    int32_t child_count = lv_obj_get_child_count(g_emoji_panel);
    for (int32_t i = child_count - 1; i >= 2; i--) {
        lv_obj_delete(lv_obj_get_child(g_emoji_panel, i));
    }

    /* Build buttons for this page */
    char img_src[256];
    for (size_t i = 0; i < page->count; i++) {
        lv_obj_t *btn = lv_btn_create(g_emoji_panel);
        lv_obj_set_size(btn, EMOJI_BTN, EMOJI_BTN);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        emoji_item_focus_states(btn, false);

        lv_obj_t *img = lv_image_create(btn);
        snprintf(img_src, sizeof(img_src), "A:%s/%s.png",
                 emoji_font_get_dir(), page->entries[i].asset);
        lv_image_set_src(img, img_src);
        lv_obj_center(img);

        /* Pass the entry pointer directly so click cb works across pages */
        lv_obj_add_event_cb(btn, emoji_click_cb, LV_EVENT_CLICKED,
                            (void *)&page->entries[i]);

        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_group_remove_obj(btn);
    }

    /* Focus the close button so gridnav has a sensible starting point */
    if (g_emoji_close_btn)
        lv_gridnav_set_focused(g_emoji_panel, g_emoji_close_btn, LV_ANIM_OFF);

    PROF_LOG(t0, "populate_emoji_panel page=%zu/%zu entries=%zu",
             page_idx + 1, EMOJI_PAGE_COUNT, page->count);
}

static void emoji_change_page(int delta)
{
    if (!g_emoji_panel) return;
    PROF_START(t0);
    int n = (int)EMOJI_PAGE_COUNT;
    int next = ((int)g_emoji_page_idx + delta) % n;
    if (next < 0) next += n;
    g_emoji_page_idx = (size_t)next;
    populate_emoji_panel(g_emoji_page_idx);
    PROF_WARN_IF_OVER(t0, 50, "emoji_change_page delta=%d", delta);
}

static void destroy_emoji_panel(void)
{
    if (!g_emoji_panel) return;
    lv_obj_delete(g_emoji_panel);   /* auto-removes from group */
    g_emoji_panel        = NULL;
    g_emoji_header_label = NULL;
    g_emoji_close_btn    = NULL;
    sync_state_from_globals(get_state(g_parent));

    /* Re-add sidebar rows to group and focus textarea.
     * Tab now cycles: sidebar rows + textarea */
    lv_group_t *grp = lv_group_get_default();
    if (grp) ui_sidebar_add_to_group(grp);
    if (g_textarea) lv_group_focus_obj(g_textarea);
}

static void emoji_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (g_emoji_panel) {
        destroy_emoji_panel();
    } else {
        create_emoji_panel();
    }
}

static void emoji_close_cb(lv_event_t *e)
{
    (void)e;
    destroy_emoji_panel();
}

/* ` + vowel -> accented vowel (lang.h). LVGL sends LV_EVENT_INSERT with
 * the text about to go in at the cursor; on a combo, delete the ` (or ``)
 * and have LVGL insert the accented letter instead. The nested delete resets the
 * insert-replace slot, so it must come first. */
static void textarea_insert_cb(lv_event_t *e)
{
    ui_chat_panel_state_t *st = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    uint32_t ticks;
    const char *accented = lang_accent_combo(st->lang, lv_textarea_get_text(ta),
                                             lv_textarea_get_cursor_pos(ta),
                                             lv_event_get_param(e), &ticks);
    if (!accented) return;
    while (ticks--) lv_textarea_delete_char(ta);
    lv_textarea_set_insert_replace(ta, accented);
}

void ui_chat_set_language(chat_lang_t lang)
{
    ui_chat_panel_state_t *st = g_parent ? get_state(g_parent) : NULL;
    if (st) st->lang = lang;
}

bool ui_chat_close_emoji_panel(void)
{
    if (!g_emoji_panel) return false;
    destroy_emoji_panel();
    return true;
}

bool ui_chat_is_open(void)    { return g_textarea != NULL; }
bool ui_chat_emoji_open(void) { return g_emoji_panel != NULL; }

void ui_chat_set_avatar(const void *src)
{
    ui_chat_panel_state_t *st = g_parent ? get_state(g_parent) : NULL;
    if (!st || !st->head_avatar) return;
    lv_image_set_src(st->head_avatar, src);
    if (src) {
        lv_label_set_text(st->head_initial, "");
        return;
    }
    /* No picture: the name's first letter (the header's name label). */
    lv_obj_t *name = lv_obj_get_sibling(lv_obj_get_parent(st->head_avatar), 1);
    const char *n = name ? lv_label_get_text(name) : "";
    char ini[8] = { 0 };
    size_t len = 1;
    unsigned char c = (unsigned char)n[0];
    if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
    for (size_t i = 0; i < len && n[i]; i++) ini[i] = n[i];
    lv_label_set_text(st->head_initial, ini);
}

static void chat_key_cb(lv_event_t *e)
{
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key == LV_KEY_MENU) {
        if (g_emoji_panel) {
            destroy_emoji_panel();
        } else {
            create_emoji_panel();
        }
        lv_event_stop_processing(e);
    } else if (key == LV_KEY_ESC) {
        if (g_emoji_panel) {
            destroy_emoji_panel();
            lv_event_stop_processing(e);
        }
    } else if (key == LV_KEY_PG_UP && g_emoji_panel) {
        emoji_change_page(-1);
        lv_event_stop_processing(e);
    } else if (key == LV_KEY_PG_DOWN && g_emoji_panel) {
        emoji_change_page(+1);
        lv_event_stop_processing(e);
    }
}

static void emoji_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const emoji_entry_t *entry = lv_event_get_user_data(e);
    if (!entry || !g_textarea) return;

    lv_textarea_add_text(g_textarea, entry->utf8);

    /* Restore gridnav focus so arrow keys keep working after insert */
    if (g_emoji_panel)
        lv_gridnav_set_focused(g_emoji_panel, btn, LV_ANIM_OFF);
}

/* -------------------------------------------------------------------------
 * Testing getters
 * ---------------------------------------------------------------------- */

#ifdef CHATBOX_TESTING
int ui_chat_test_photo_bubbles(int *with_picture)
{
    int n = 0, pic = 0;
    for (photo_bubble_t *b = g_photos; b; b = b->next) {
        if (b->msg_cont != g_msg_cont) continue;
        n++;
        if (ui_photo_img_ok(&b->pic)) pic++;
    }
    if (with_picture) *with_picture = pic;
    return n;
}

void ui_chat_test_flush_photos(void)
{
    photo_bubble_t *b;
    while ((b = photo_next()) != NULL) photo_bubble_load(b);
}

bool ui_chat_test_photo_shows(const char *event_id)
{
    for (photo_bubble_t *b = g_photos; b; b = b->next)
        if (strcmp(b->event_id, event_id) == 0 && ui_photo_img_ok(&b->pic)) return true;
    return false;
}

size_t ui_chat_test_photo_bytes(void) { return g_photo_bytes; }

lv_obj_t *ui_chat_get_textarea(void)      { return g_textarea; }
lv_obj_t *ui_chat_get_send_btn(void)      { return g_send_btn; }
lv_obj_t *ui_chat_get_emoji_panel(void)   { return g_emoji_panel; }
lv_obj_t *ui_chat_get_msg_cont(void)      { return g_msg_cont; }
lv_obj_t *ui_chat_get_new_divider(void)   { return g_new_divider; }
size_t      ui_chat_get_emoji_page_idx(void)   { return g_emoji_page_idx; }
size_t      ui_chat_get_emoji_page_count(void) { return EMOJI_PAGE_COUNT; }
const char *ui_chat_get_emoji_page_title(void) {
    return g_emoji_pages[g_emoji_page_idx].title;
}
size_t      ui_chat_get_emoji_page_entry_count(void) {
    return g_emoji_pages[g_emoji_page_idx].count;
}
#endif
