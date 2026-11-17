/**
 * test_keyboard.c — Headless keyboard navigation tests
 *
 * Standalone binary that initializes LVGL with a dummy display, builds the UI,
 * and exercises keyboard navigation. Uses a hybrid approach:
 * - NEXT/PREV: direct group focus calls (bypass indev timing issues)
 * - ENTER: virtual indev press/release for realistic CLICKED event chain
 * - Other keys (MENU, ESC, arrows): lv_group_send_data for KEY events
 *
 * Compile with: -DCHATBOX_TESTING
 * Run: ./test_keyboard [config_path]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "config.h"
#include "ui_sidebar.h"
#include "ui_chat.h"
#include "emoji_font.h"
#include "font_chat.h"
#include "display_sleep.h"
#include "app.h"
#include "shell.h"
#include "switcher.h"
#include "key_filter.h"
#include "avatars.h"
#include "ui_photos.h"
#include "ui_theme.h"
#include "photo_lib.h"
#include "photo_inbox.h"
#include "test_photo_fixture.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

/* Custom control codes — same as defined in ui_chat.c */
#define LV_KEY_MENU    0x04
#define LV_KEY_PG_UP   0x05
#define LV_KEY_PG_DOWN 0x06

/* Test counters (shared with test_music_fixture.c when it is linked) */
#ifdef CHATBOX_HAVE_SQLITE
#include "test_music_fixture.h"
#else
static int g_tests   = 0;
static int g_failures = 0;
#endif

/* References to layout panels (mirror main.c) */
static lv_obj_t *g_sidebar_panel  = NULL;
static lv_obj_t *g_content_panel  = NULL;
static config_t  g_cfg;
static int        g_current_contact = -1;

/* Virtual keypad indev for ENTER simulation */
static lv_indev_t *g_test_indev = NULL;
static uint32_t    g_pending_key = 0;
static lv_indev_state_t g_pending_state = LV_INDEV_STATE_RELEASED;

/* -------------------------------------------------------------------------
 * LVGL Headless Setup
 * ---------------------------------------------------------------------- */

/* Fake clock for the shell groups (12+), which need exact LVGL repeat and
 * key-filter timing. Off for the original groups, which use real time. */
static bool     g_fake_clock = false;
static uint32_t g_fake_now   = 0;

static uint32_t test_tick_cb(void)
{
    if (g_fake_clock) return g_fake_now;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The whole frame, kept by the flush callback so a test can save a
 * snapshot of the screen (XRGB8888, as LV_COLOR_DEPTH 32 renders it). */
#define TEST_SCR_W 1280
#define TEST_SCR_H 800
static uint32_t g_frame[TEST_SCR_W * TEST_SCR_H];

static void dummy_flush_cb(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    const uint32_t *src = (const uint32_t *)px_map;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= TEST_SCR_H) { src += w; continue; }
        for (int32_t x = area->x1; x <= area->x2; x++, src++)
            if (x >= 0 && x < TEST_SCR_W) g_frame[y * TEST_SCR_W + x] = *src;
    }
    lv_display_flush_ready(disp);
}

static void test_keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->key = g_pending_key;
    data->state = g_pending_state;
}

/* -------------------------------------------------------------------------
 * Key injection helpers
 * ---------------------------------------------------------------------- */

/* Force-read the virtual indev */
static void force_indev_read(void)
{
    lv_timer_t *timer = lv_indev_get_read_timer(g_test_indev);
    if (timer) lv_indev_read_timer_cb(timer);
}

/* Navigate to next/prev in the default group.
 * Clears editing mode first (same as LVGL's KEYPAD NEXT handler). */
static void press_next(void)
{
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_set_editing(grp, false);
        lv_group_focus_next(grp);
    }
    lv_timer_handler();
}


/* Send a key event (MENU, ESC, arrows) to the focused object */
/* Let `ms` of fake time pass (theme transitions, animations). */
static void settle_ms(uint32_t ms)
{
    for (; ms >= 4; ms -= 4) {
        if (g_fake_clock) g_fake_now += 4;
        else usleep(4000);
        lv_timer_handler();
    }
}

static void press_key(uint32_t key)
{
    lv_group_t *grp = lv_group_get_default();
    if (!grp) return;
    lv_group_send_data(grp, key);
    lv_timer_handler();
}

/* Simulate ENTER with full press/release via the indev pipeline.
 * This triggers PRESSED, SHORT_CLICKED, CLICKED, RELEASED events
 * which is needed for button clicks and row selections.
 * After processing, reset the indev to clear stale tracking state
 * (important when ENTER callbacks modify the group structure). */
static void press_enter(void)
{
    g_pending_key = LV_KEY_ENTER;
    g_pending_state = LV_INDEV_STATE_PRESSED;
    force_indev_read();
    g_pending_state = LV_INDEV_STATE_RELEASED;
    force_indev_read();
    lv_timer_handler();
    /* Reset indev to clear stale object tracking */
    lv_indev_reset(g_test_indev, NULL);
}

/* -------------------------------------------------------------------------
 * Assertion helpers
 * ---------------------------------------------------------------------- */

static void assert_focused(lv_obj_t *expected, const char *desc)
{
    g_tests++;
    lv_group_t *grp = lv_group_get_default();
    lv_obj_t *actual = grp ? lv_group_get_focused(grp) : NULL;
    if (actual != expected) {
        fprintf(stderr, "[FAIL] %s: expected %p, got %p\n", desc,
                (void *)expected, (void *)actual);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_not_null(const void *ptr, const char *desc)
{
    g_tests++;
    if (!ptr) {
        fprintf(stderr, "[FAIL] %s: expected non-NULL, got NULL\n", desc);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_null(const void *ptr, const char *desc)
{
    g_tests++;
    if (ptr) {
        fprintf(stderr, "[FAIL] %s: expected NULL, got %p\n", desc, ptr);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_textarea_contains(const char *expected, const char *desc)
{
    g_tests++;
    lv_obj_t *ta = ui_chat_get_textarea();
    const char *text = ta ? lv_textarea_get_text(ta) : NULL;
    if (!text || !strstr(text, expected)) {
        fprintf(stderr, "[FAIL] %s: textarea='%s' expected to contain '%s'\n",
                desc, text ? text : "(null)", expected);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_textarea_empty(const char *desc)
{
    g_tests++;
    lv_obj_t *ta = ui_chat_get_textarea();
    const char *text = ta ? lv_textarea_get_text(ta) : NULL;
    if (text && text[0] != '\0') {
        fprintf(stderr, "[FAIL] %s: textarea='%s' expected empty\n", desc, text);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_true(bool cond, const char *desc)
{
    g_tests++;
    if (!cond) {
        fprintf(stderr, "[FAIL] %s\n", desc);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_hidden(lv_obj_t *obj, const char *desc)
{
    g_tests++;
    if (!obj || !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        fprintf(stderr, "[FAIL] %s: expected HIDDEN\n", desc);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_visible(lv_obj_t *obj, const char *desc)
{
    g_tests++;
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        fprintf(stderr, "[FAIL] %s: expected visible (not HIDDEN)\n", desc);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

/* -------------------------------------------------------------------------
 * Layout builder (mirrors main.c but without Matrix API / emoji font)
 * ---------------------------------------------------------------------- */

static void noop_send(const char *text, void *send_ctx)
{
    (void)text;
    /* Release the pending slot ui_chat reserved for this bubble, so
     * the test's repeated sends don't exhaust the pending table. */
    ui_chat_pending_complete(send_ctx, false);
}

static void show_chat(int contact_idx);

static void on_contact_selected(int contact_idx)
{
    show_chat(contact_idx);
}

static void build_layout(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_gap(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_sidebar_panel = lv_obj_create(scr);
    lv_obj_set_style_pad_all(g_sidebar_panel, 0, 0);
    lv_obj_set_style_border_width(g_sidebar_panel, 0, 0);
    lv_obj_set_style_radius(g_sidebar_panel, 0, 0);
    ui_sidebar_create(g_sidebar_panel, &g_cfg, on_contact_selected);

    g_content_panel = lv_obj_create(scr);
    lv_obj_set_flex_grow(g_content_panel, 1);
    lv_obj_set_height(g_content_panel, lv_pct(100));
    lv_obj_set_style_bg_opa(g_content_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_content_panel, 0, 0);
    lv_obj_set_style_radius(g_content_panel, 0, 0);
    lv_obj_set_style_pad_all(g_content_panel, 0, 0);
    lv_obj_clear_flag(g_content_panel, LV_OBJ_FLAG_SCROLLABLE);
}

static void show_chat(int contact_idx)
{
    if (contact_idx < 0 || contact_idx >= g_cfg.contact_count) return;
    if (contact_idx == g_current_contact) return;

    ui_chat_destroy();
    g_current_contact = contact_idx;
    ui_sidebar_set_unread(contact_idx, false);
    ui_sidebar_set_selected(contact_idx);

    lv_obj_clean(g_content_panel);
    ui_chat_create(g_content_panel, g_cfg.contacts[contact_idx].name,
                   g_cfg.contacts[contact_idx].room_id);
    ui_chat_set_avatar(ui_sidebar_avatar_src(contact_idx));
    ui_chat_set_send_callback(noop_send);
}

/* Assign test indev to the current default group */
static void sync_indev(void)
{
    lv_group_t *grp = lv_group_get_default();
    if (grp && g_test_indev)
        lv_indev_set_group(g_test_indev, grp);
}

static void reset_all(void)
{
    g_current_contact = -1;
    ui_chat_destroy();

    /* Remove all objects from group BEFORE cleaning the screen.
     * This prevents focus-change callbacks (row_focus_cb → show_chat)
     * from firing during object destruction and accessing freed widgets. */
    lv_group_t *grp = lv_group_get_default();
    if (grp) {
        lv_group_remove_all_objs(grp);
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    if (grp) {
        lv_group_delete(grp);
        lv_group_set_default(NULL);
    }

    lv_timer_handler();
}

/* -------------------------------------------------------------------------
 * Test Groups
 * ---------------------------------------------------------------------- */

static void test_group1_sidebar_navigation(void)
{
    fprintf(stderr, "\n=== Group 1: Sidebar Navigation ===\n");

    reset_all();
    build_layout();
    lv_timer_handler();
    sync_indev();

    /* T1.1: the kiosk boots into the first contact's chat (app_chat.c's
     * app_chat_open_first goes through ui_sidebar_open, as a row does). */
    ui_sidebar_open(0);
    lv_timer_handler();
    assert_true(g_current_contact == 0, "T1.1a: contact 0's chat opens");
    assert_not_null(ui_chat_get_textarea(), "T1.1b: chat textarea created");
    assert_not_null(ui_chat_get_msg_cont(), "T1.1c: msg container created");

    /* T1.2: tapping a contact's avatar opens their chat. */
    lv_obj_send_event(ui_sidebar_get_row(1), LV_EVENT_CLICKED, NULL);
    lv_timer_handler();
    assert_true(g_current_contact == 1, "T1.2a: tap on contact 1 opens their chat");
    lv_obj_send_event(ui_sidebar_get_row(0), LV_EVENT_CLICKED, NULL);
    lv_timer_handler();
    assert_true(g_current_contact == 0, "T1.2b: tap on contact 0 opens theirs again");
    lv_group_focus_obj(ui_chat_get_textarea());
    lv_timer_handler();
}

static void test_group2_chat_navigation(void)
{
    fprintf(stderr, "\n=== Group 2: Chat Navigation (No Emoji Panel) ===\n");

    /* After group 1 opened contact 0's chat, chat should be up.
     * Re-sync state: focus textarea, reassign indev. */
    sync_indev();
    lv_obj_t *textarea = ui_chat_get_textarea();
    lv_obj_t *send_btn = ui_chat_get_send_btn();

    /* Group should contain sidebar rows + textarea (not send_btn) */

    if (textarea) lv_group_focus_obj(textarea);
    lv_timer_handler();

    /* T2.1: Tab from textarea auto-switches to contact and focuses new textarea.
     * Tab to row[0] triggers auto-switch → show_chat(0) → textarea focused. */
    assert_focused(textarea, "T2.1a: textarea focused after chat create");
    /* Tab triggers auto-switch: lands on sidebar row → row_focus_cb fires
     * → show_chat() rebuilds chat → new textarea created.
     * The group is rebuilt during the focus event, so we verify the chat
     * was switched by checking a new textarea exists. */
    press_next();
    lv_timer_handler();
    sync_indev();
    textarea = ui_chat_get_textarea();
    assert_not_null(textarea, "T2.1b: Tab auto-switched, new textarea exists");
    /* Focus the textarea explicitly (group rebuild during event may leave
     * focus on the row; the real LVGL loop settles this naturally) */
    lv_group_focus_obj(textarea);
    lv_timer_handler();

    /* T2.2: Send button NOT reachable via Tab */
    send_btn = ui_chat_get_send_btn();
    bool send_focused = false;
    for (int i = 0; i < 6; i++) {
        press_next();
        lv_group_t *grp2 = lv_group_get_default();
        lv_obj_t *foc = grp2 ? lv_group_get_focused(grp2) : NULL;
        if (foc == send_btn) {
            send_focused = true;
            break;
        }
    }
    assert_true(!send_focused, "T2.2: send button never focused in chat mode");

    /* T2.3: Enter on textarea sends (clears text) */
    textarea = ui_chat_get_textarea();
    lv_group_focus_obj(textarea);
    lv_timer_handler();
    lv_textarea_add_text(textarea, "hello");
    assert_textarea_contains("hello", "T2.3a: textarea has 'hello'");
    press_enter();
    assert_textarea_empty("T2.3b: textarea cleared after Enter send");

    /* T2.4: Enter sends another message */
    textarea = ui_chat_get_textarea();
    lv_group_focus_obj(textarea);
    lv_timer_handler();
    lv_textarea_add_text(textarea, "test2");
    assert_textarea_contains("test2", "T2.4a: textarea has 'test2'");
    press_enter();
    assert_textarea_empty("T2.4b: textarea cleared after second send");
}

static void test_group3_emoji_open_close(void)
{
    fprintf(stderr, "\n=== Group 3: Emoji Panel Open/Close ===\n");

    lv_obj_t *textarea = ui_chat_get_textarea();
    lv_group_focus_obj(textarea);
    lv_timer_handler();

    /* T3.1: Menu key opens emoji panel */
    assert_null(ui_chat_get_emoji_panel(), "T3.1a: emoji panel initially NULL");
    press_key(LV_KEY_MENU);
    assert_not_null(ui_chat_get_emoji_panel(), "T3.1b: emoji panel created after Menu");
    assert_focused(ui_chat_get_emoji_panel(), "T3.1c: emoji panel focused");

    /* T3.2: Menu key toggles (closes if open) */
    press_key(LV_KEY_MENU);
    assert_null(ui_chat_get_emoji_panel(), "T3.2a: Menu closes emoji panel");
    assert_focused(textarea, "T3.2b: focus returns to textarea");

    /* T3.3: Esc closes emoji panel */
    press_key(LV_KEY_MENU); /* open */
    assert_not_null(ui_chat_get_emoji_panel(), "T3.3a: panel opened");
    press_key(LV_KEY_ESC);
    assert_null(ui_chat_get_emoji_panel(), "T3.3b: Esc closes panel");
    assert_focused(textarea, "T3.3c: focus returns to textarea after Esc");

    /* T3.4: Esc does nothing when panel is closed */
    assert_null(ui_chat_get_emoji_panel(), "T3.4a: panel already closed");
    press_key(LV_KEY_ESC);
    assert_null(ui_chat_get_emoji_panel(), "T3.4b: panel still closed after Esc");
    assert_focused(textarea, "T3.4c: textarea still focused");

    /* T3.5: Send auto-closes emoji panel */
    press_key(LV_KEY_MENU); /* open */
    assert_not_null(ui_chat_get_emoji_panel(), "T3.5a: panel opened");

    /* Tab to textarea */
    press_next();
    textarea = ui_chat_get_textarea();
    assert_focused(textarea, "T3.5b: tabbed to textarea");
    lv_textarea_add_text(textarea, "hi");
    press_enter(); /* send */
    assert_null(ui_chat_get_emoji_panel(), "T3.5c: emoji panel closed after send");
    assert_textarea_empty("T3.5d: textarea cleared after send");
}

static void test_group4_tab_with_emoji(void)
{
    fprintf(stderr, "\n=== Group 4: Tab with Emoji Panel Open ===\n");

    /* Ensure clean state: no emoji panel, textarea focused and empty */
    if (ui_chat_get_emoji_panel()) press_key(LV_KEY_ESC);
    lv_obj_t *textarea = ui_chat_get_textarea();
    if (textarea) lv_textarea_set_text(textarea, "");
    lv_group_focus_obj(textarea);
    lv_timer_handler();

    /* T4.1: Tab bounces between emoji panel and textarea only */
    press_key(LV_KEY_MENU); /* open */
    assert_not_null(ui_chat_get_emoji_panel(), "T4.1a: emoji panel open");
    assert_focused(ui_chat_get_emoji_panel(), "T4.1b: emoji panel focused");
    press_next();
    textarea = ui_chat_get_textarea();
    assert_focused(textarea, "T4.1c: Tab -> textarea");
    press_next();
    assert_focused(ui_chat_get_emoji_panel(), "T4.1d: Tab -> emoji panel");

    /* T4.2: Sidebar rows NOT in group when panel is open */
    lv_obj_t *r0 = ui_sidebar_get_row(0);
    lv_obj_t *r1 = ui_sidebar_get_row(1);
    bool sidebar_focused = false;
    for (int i = 0; i < 10; i++) {
        press_next();
        lv_group_t *grp2 = lv_group_get_default();
        lv_obj_t *foc = grp2 ? lv_group_get_focused(grp2) : NULL;
        if (foc == r0 || foc == r1) {
            sidebar_focused = true;
            break;
        }
    }
    assert_true(!sidebar_focused, "T4.2: sidebar rows never focused with emoji panel open");

    /* Clean up */
    press_key(LV_KEY_ESC);
}

static void test_group5_emoji_grid_nav(void)
{
    fprintf(stderr, "\n=== Group 5: Emoji Picker Arrow Navigation ===\n");

    /* Ensure clean state */
    if (ui_chat_get_emoji_panel()) press_key(LV_KEY_ESC);
    lv_obj_t *textarea = ui_chat_get_textarea();
    if (textarea) lv_textarea_set_text(textarea, "");
    lv_group_focus_obj(textarea);
    lv_timer_handler();

    /* T5.1: Arrows navigate emoji grid */
    press_key(LV_KEY_MENU); /* open */
    lv_obj_t *panel = ui_chat_get_emoji_panel();
    assert_not_null(panel, "T5.1a: emoji panel open");
    assert_focused(panel, "T5.1b: emoji panel focused");

    /* T5.1f: a whole page fits the sheet: nothing below to scroll to. */
    lv_obj_update_layout(panel);
    assert_true(lv_obj_get_scroll_bottom(panel) <= 0,
                "T5.1f: the emoji page fits the sheet (no 4th row)");

    /* T5.1d: Down from the × lands on the emoji under it, Up comes back.
     * The focused item's sticker lift must not fool gridnav's geometry. */
    {
        lv_obj_t *close = lv_obj_get_child(panel, 1);
        lv_obj_t *below = NULL;
        settle_ms(300);                   /* the theme animates the lift in */
        for (uint32_t i = 2; i < lv_obj_get_child_count(panel); i++) {
            lv_obj_t *c = lv_obj_get_child(panel, (int32_t)i);
            if (lv_obj_get_y(c) > lv_obj_get_y(close) + 45 &&
                (!below || abs(lv_obj_get_x(c) - lv_obj_get_x(close)) <
                           abs(lv_obj_get_x(below) - lv_obj_get_x(close))))
                below = c;
        }
        assert_not_null(below, "T5.1d: a row under the ×");
        press_key(LV_KEY_DOWN);
        settle_ms(300);
        assert_true(lv_obj_has_state(below, LV_STATE_FOCUSED),
                    "T5.1d: Down from × focuses the emoji under it");
        press_key(LV_KEY_UP);
        assert_true(lv_obj_has_state(close, LV_STATE_FOCUSED),
                    "T5.1e: Up comes back to the ×");
    }

    /* Press right to move from close button (child 0) to first emoji button */
    press_key(LV_KEY_RIGHT);
    assert_focused(panel, "T5.1c: panel still group-focused after Right");

    /* T5.2: Enter selects emoji and inserts into textarea.
     * Emoji panel children: [0]=header_label, [1]=close_btn,
     *                       [2]=emoji[0], [3]=emoji[1], ...
     * Click the first emoji directly since gridnav forwarding requires an
     * active indev. */
    {
        lv_obj_t *emoji_btn = lv_obj_get_child(panel, 2);
        lv_obj_send_event(emoji_btn, LV_EVENT_CLICKED, NULL);
    }
    lv_timer_handler();
    /* First emoji on Favorites page: U+1F600 grinning face */
    assert_textarea_contains("\xF0\x9F\x98\x80",
                             "T5.2: Enter inserts emoji into textarea");

    /* T5.3: Arrow nav still works after Enter (the bug fix).
     * emoji_click_cb calls lv_gridnav_set_focused to restore internal focus. */
    press_key(LV_KEY_RIGHT);
    assert_focused(ui_chat_get_emoji_panel(),
                   "T5.3: panel still focused, arrow nav works after Enter");

    /* T5.4: Multiple emoji selected sequentially.
     * Second emoji is at child[3]. */
    {
        lv_obj_t *emoji_btn2 = lv_obj_get_child(panel, 3);
        lv_obj_send_event(emoji_btn2, LV_EVENT_CLICKED, NULL);
    }
    lv_timer_handler();
    /* Second emoji on Favorites page: U+1F602 tears of joy */
    assert_textarea_contains("\xF0\x9F\x98\x82",
                             "T5.4: second emoji inserted");

    /* Clean up */
    press_key(LV_KEY_ESC);
    lv_textarea_set_text(textarea, "");
}

static void assert_size_eq(size_t actual, size_t expected, const char *desc)
{
    g_tests++;
    if (actual != expected) {
        fprintf(stderr, "[FAIL] %s: expected %zu, got %zu\n",
                desc, expected, actual);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void assert_str_eq(const char *actual, const char *expected,
                          const char *desc)
{
    g_tests++;
    if (!actual || strcmp(actual, expected) != 0) {
        fprintf(stderr, "[FAIL] %s: expected '%s', got '%s'\n",
                desc, expected, actual ? actual : "(null)");
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] %s\n", desc);
    }
}

static void test_group10_emoji_pages(void)
{
    fprintf(stderr, "\n=== Group 10: Emoji Picker Pagination ===\n");

    /* Clean state: panel closed, textarea empty + focused */
    if (ui_chat_get_emoji_panel()) press_key(LV_KEY_ESC);
    lv_obj_t *textarea = ui_chat_get_textarea();
    if (textarea) lv_textarea_set_text(textarea, "");
    lv_group_focus_obj(textarea);
    lv_timer_handler();

    size_t total = ui_chat_get_emoji_page_count();

    /* T10.1: Open lands on the page index that was last set (initially 0). */
    press_key(LV_KEY_MENU);
    assert_not_null(ui_chat_get_emoji_panel(), "T10.1a: panel open");
    assert_size_eq(ui_chat_get_emoji_page_idx(), 0, "T10.1b: starts on page 0");
    assert_str_eq(ui_chat_get_emoji_page_title(), "Favorites",
                  "T10.1c: page 0 title is Favorites");

    /* T10.2: PageDown advances to page 1. */
    press_key(LV_KEY_PG_DOWN);
    assert_size_eq(ui_chat_get_emoji_page_idx(), 1, "T10.2a: PG_DOWN → page 1");
    assert_str_eq(ui_chat_get_emoji_page_title(), "Smileys & People",
                  "T10.2b: page 1 title");

    /* T10.3: PageUp from page 1 returns to page 0. */
    press_key(LV_KEY_PG_UP);
    assert_size_eq(ui_chat_get_emoji_page_idx(), 0, "T10.3: PG_UP → page 0");

    /* T10.4: PageUp from page 0 wraps to last page. */
    press_key(LV_KEY_PG_UP);
    assert_size_eq(ui_chat_get_emoji_page_idx(), total - 1,
                   "T10.4a: PG_UP wraps to last page");
    /* T10.5: PageDown from last page wraps to page 0. */
    press_key(LV_KEY_PG_DOWN);
    assert_size_eq(ui_chat_get_emoji_page_idx(), 0,
                   "T10.5: PG_DOWN wraps to page 0");

    /* T10.6: Page index persists across close + reopen. */
    press_key(LV_KEY_PG_DOWN);
    press_key(LV_KEY_PG_DOWN);
    size_t before = ui_chat_get_emoji_page_idx();
    assert_size_eq(before, 2, "T10.6a: at page 2");
    press_key(LV_KEY_ESC);
    assert_null(ui_chat_get_emoji_panel(), "T10.6b: panel closed");
    press_key(LV_KEY_MENU);
    assert_size_eq(ui_chat_get_emoji_page_idx(), 2,
                   "T10.6c: reopened at page 2");

    /* T10.7: Each page renders its own emoji buttons. */
    lv_obj_t *panel = ui_chat_get_emoji_panel();
    /* Children: header label + close button + emoji buttons */
    size_t expected = 2 + ui_chat_get_emoji_page_entry_count();
    assert_size_eq((size_t)lv_obj_get_child_count(panel), expected,
                   "T10.7: child count matches header+close+entries");

    /* T10.8: PG_UP/PG_DOWN no-op when panel is closed. */
    press_key(LV_KEY_ESC);
    assert_null(ui_chat_get_emoji_panel(), "T10.8a: panel closed");
    size_t idx_before = ui_chat_get_emoji_page_idx();
    press_key(LV_KEY_PG_DOWN);
    assert_null(ui_chat_get_emoji_panel(),
                "T10.8b: PG_DOWN with no panel does nothing");
    assert_size_eq(ui_chat_get_emoji_page_idx(), idx_before,
                   "T10.8c: page index unchanged");

    /* Reset to page 0 for any later groups. */
    press_key(LV_KEY_MENU);
    while (ui_chat_get_emoji_page_idx() != 0) press_key(LV_KEY_PG_UP);
    press_key(LV_KEY_ESC);
    if (textarea) lv_textarea_set_text(textarea, "");
    lv_group_focus_obj(textarea);
    lv_timer_handler();
}

static void test_group6_contact_switching(void)
{
    fprintf(stderr, "\n=== Group 6: Contact Switching ===\n");

    /* T6.1: Switching contacts rebuilds chat group */
    show_chat(1);
    lv_timer_handler();
    sync_indev();

    lv_obj_t *textarea = ui_chat_get_textarea();
    /* Explicitly focus textarea — same as real app, where the first group
     * member auto-focuses. The test indev state may have interfered. */
    if (textarea) lv_group_focus_obj(textarea);
    lv_timer_handler();
    assert_focused(textarea, "T6.1a: textarea focused after contact switch");

    /* Tab should auto-switch to the other contact (skipping the current one).
     * After show_chat(1), row1 is selected, so Tab skips row1 and hits row0. */
    lv_obj_t *row0 = ui_sidebar_get_row(0);
    lv_obj_t *row1 = ui_sidebar_get_row(1);
    bool other_contact_reached = false;
    for (int i = 0; i < 10; i++) {
        press_next();
        lv_timer_handler();
        lv_group_t *grp2 = lv_group_get_default();
        lv_obj_t *foc = grp2 ? lv_group_get_focused(grp2) : NULL;
        /* After auto-switch, textarea of the new contact should be focused */
        textarea = ui_chat_get_textarea();
        if (foc == textarea || foc == row0) {
            other_contact_reached = true;
            break;
        }
    }
    assert_true(other_contact_reached, "T6.1b: Tab reaches other contact after switch");

    /* T6.2: Tab skips current contact, switches to next in one press */
    g_current_contact = -1;
    show_chat(0);  /* open contact 0 */
    lv_timer_handler();
    sync_indev();
    textarea = ui_chat_get_textarea();
    lv_group_focus_obj(textarea);
    lv_timer_handler();

    /* Tab should skip row0 (current) and auto-switch to contact 1.
     * The lv_group_focus_next() inside row_focus_cb may need an extra
     * timer cycle to settle, so we press_next twice if needed. */
    press_next();
    lv_timer_handler();
    if (g_current_contact == 0) {
        /* Skip triggered focus_next but event hasn't settled — nudge */
        press_next();
        lv_timer_handler();
    }
    assert_true(g_current_contact == 1,
                "T6.2: Tab switched to contact 1 (skipped current)");

    /* T6.3: Emoji panel closes on contact switch */
    textarea = ui_chat_get_textarea();
    lv_group_focus_obj(textarea);
    lv_timer_handler();
    press_key(LV_KEY_MENU);
    assert_not_null(ui_chat_get_emoji_panel(), "T6.3a: emoji panel open");

    g_current_contact = -1; /* allow re-switch */
    show_chat(0);
    lv_timer_handler();
    assert_null(ui_chat_get_emoji_panel(), "T6.3b: emoji panel closed after contact switch");
}

static void test_group7_unread_dots(void)
{
    fprintf(stderr, "\n=== Group 7: Sidebar Unread Dots ===\n");

    reset_all();
    build_layout();
    lv_timer_handler();
    sync_indev();

    lv_obj_t *dot0 = ui_sidebar_get_dot(0);
    lv_obj_t *dot1 = ui_sidebar_get_dot(1);

    /* T7.1: Dots exist but are hidden by default */
    assert_not_null(dot0, "T7.1a: dot[0] exists");
    assert_not_null(dot1, "T7.1b: dot[1] exists");
    assert_hidden(dot0, "T7.1c: dot[0] hidden initially");
    assert_hidden(dot1, "T7.1d: dot[1] hidden initially");

    /* T7.2: set_unread(true) shows the dot */
    ui_sidebar_set_unread(0, true);
    assert_visible(dot0, "T7.2a: dot[0] visible after set_unread(true)");
    assert_hidden(dot1, "T7.2b: dot[1] still hidden");

    /* T7.3: Multiple contacts can have unread dots */
    ui_sidebar_set_unread(1, true);
    assert_visible(dot0, "T7.3a: dot[0] still visible");
    assert_visible(dot1, "T7.3b: dot[1] now visible");

    /* T7.4: set_unread(false) hides the dot */
    ui_sidebar_set_unread(0, false);
    assert_hidden(dot0, "T7.4a: dot[0] hidden after set_unread(false)");
    assert_visible(dot1, "T7.4b: dot[1] still visible");

    /* T7.5: Opening a contact's chat clears its unread dot */
    ui_sidebar_set_unread(0, true);
    ui_sidebar_set_unread(1, true);
    assert_visible(dot0, "T7.5a: dot[0] visible before switch");
    assert_visible(dot1, "T7.5b: dot[1] visible before switch");

    show_chat(0);
    lv_timer_handler();
    dot0 = ui_sidebar_get_dot(0);
    dot1 = ui_sidebar_get_dot(1);
    assert_hidden(dot0, "T7.5c: dot[0] cleared after opening contact 0");
    assert_visible(dot1, "T7.5d: dot[1] still visible (different contact)");

    /* T7.6: Out-of-bounds index is a no-op (no crash) */
    ui_sidebar_set_unread(-1, true);
    ui_sidebar_set_unread(99, true);
    assert_true(true, "T7.6: out-of-bounds set_unread did not crash");
}

static void test_group8_new_divider(void)
{
    fprintf(stderr, "\n=== Group 8: New Messages Divider ===\n");

    reset_all();
    build_layout();
    lv_timer_handler();
    sync_indev();

    /* Open a chat first */
    show_chat(0);
    lv_timer_handler();

    /* T8.1: Divider initially NULL */
    assert_null(ui_chat_get_new_divider(), "T8.1: divider NULL initially");

    /* T8.2: insert_new_divider creates the divider */
    ui_chat_insert_new_divider();
    lv_obj_t *div1 = ui_chat_get_new_divider();
    assert_not_null(div1, "T8.2: divider created after insert");

    /* T8.3: Calling insert again is idempotent (same widget) */
    ui_chat_insert_new_divider();
    lv_obj_t *div2 = ui_chat_get_new_divider();
    assert_true(div1 == div2, "T8.3: second insert returns same divider");

    /* T8.4: Messages can be added after divider (no crash) */
    ui_chat_add_message("hello after divider", false);
    lv_timer_handler();
    assert_not_null(ui_chat_get_new_divider(), "T8.4: divider still present after message");

    /* T8.5: clear_new_divider removes it */
    ui_chat_clear_new_divider();
    assert_null(ui_chat_get_new_divider(), "T8.5: divider NULL after clear");

    /* T8.6: Clearing when no divider is a no-op (no crash) */
    ui_chat_clear_new_divider();
    assert_null(ui_chat_get_new_divider(), "T8.6: double clear is safe");

    /* T8.7: Divider destroyed when switching contacts */
    ui_chat_insert_new_divider();
    assert_not_null(ui_chat_get_new_divider(), "T8.7a: divider present before switch");
    g_current_contact = -1; /* allow re-switch */
    show_chat(1);
    lv_timer_handler();
    assert_null(ui_chat_get_new_divider(), "T8.7b: divider gone after contact switch");

    /* T8.8: Divider works in fresh chat */
    ui_chat_insert_new_divider();
    assert_not_null(ui_chat_get_new_divider(), "T8.8: divider works in new chat");
    ui_chat_clear_new_divider();
}

/* -------------------------------------------------------------------------
 * Group 9: Display Sleep State Machine
 * ---------------------------------------------------------------------- */

static void test_group9_display_sleep(void)
{
    fprintf(stderr, "\n=== Group 9: Display Sleep State Machine ===\n");

    /* Use 1-second timeouts for fast testing */
    display_sleep_init(1, 2);
    lv_timer_handler();

    /* T9.1: starts ACTIVE, overlay hidden */
    assert_true(display_sleep_get_state() == 0, "T9.1a: initial state is ACTIVE");
    assert_true(lv_obj_has_flag(display_sleep_get_overlay(), LV_OBJ_FLAG_HIDDEN),
                "T9.1b: overlay hidden initially");
    assert_true(!display_sleep_is_locked(), "T9.1c: is_locked false initially");

    /* T9.2: after idle >= 1s, transitions to LOCKED */
    usleep(1100 * 1000);  /* 1.1 seconds */
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1, "T9.2a: state is LOCKED after idle");
    assert_true(!lv_obj_has_flag(display_sleep_get_overlay(), LV_OBJ_FLAG_HIDDEN),
                "T9.2b: overlay visible when LOCKED");
    assert_true(display_sleep_is_locked(), "T9.2c: is_locked true when LOCKED");

    /* T9.3: notify_message while LOCKED shows label and stays LOCKED */
    assert_true(lv_obj_has_flag(display_sleep_get_msg_label(), LV_OBJ_FLAG_HIDDEN),
                "T9.3a: msg label hidden before notify");
    display_sleep_notify_message();
    assert_true(!lv_obj_has_flag(display_sleep_get_msg_label(), LV_OBJ_FLAG_HIDDEN),
                "T9.3b: msg label visible after notify");
    assert_true(display_sleep_get_state() == 1, "T9.3c: still LOCKED after notify");

    /* T9.4: keypress while LOCKED → ACTIVE */
    g_pending_key = LV_KEY_ESC;
    g_pending_state = LV_INDEV_STATE_PRESSED;
    force_indev_read();
    g_pending_state = LV_INDEV_STATE_RELEASED;
    force_indev_read();
    lv_timer_handler();
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 0, "T9.4a: ACTIVE after keypress");
    assert_true(lv_obj_has_flag(display_sleep_get_overlay(), LV_OBJ_FLAG_HIDDEN),
                "T9.4b: overlay hidden after wake");
    assert_true(lv_obj_has_flag(display_sleep_get_msg_label(), LV_OBJ_FLAG_HIDDEN),
                "T9.4c: msg label hidden after wake");

    /* T9.5: LOCKED → OFF after off timeout */
    usleep(1100 * 1000);  /* idle → LOCKED */
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1, "T9.5a: LOCKED again");
    usleep(2100 * 1000);  /* 2.1s in lock → OFF */
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 2, "T9.5b: state is OFF");
    assert_true(display_sleep_is_locked(), "T9.5c: is_locked true when OFF");

    /* T9.6: notify_message while OFF → LOCKED with label */
    display_sleep_notify_message();
    assert_true(display_sleep_get_state() == 1, "T9.6a: OFF → LOCKED on message");
    assert_true(!lv_obj_has_flag(display_sleep_get_msg_label(), LV_OBJ_FLAG_HIDDEN),
                "T9.6b: msg label visible after message wake");
    assert_true(!lv_obj_has_flag(display_sleep_get_overlay(), LV_OBJ_FLAG_HIDDEN),
                "T9.6c: overlay visible after message wake");

    /* T9.7: keypress while OFF → ACTIVE */
    usleep(2100 * 1000);  /* let it go to OFF again */
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 2, "T9.7a: back to OFF");
    g_pending_key = LV_KEY_ENTER;
    g_pending_state = LV_INDEV_STATE_PRESSED;
    force_indev_read();
    g_pending_state = LV_INDEV_STATE_RELEASED;
    force_indev_read();
    lv_timer_handler();
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 0, "T9.7b: ACTIVE after keypress from OFF");
    assert_true(lv_obj_has_flag(display_sleep_get_overlay(), LV_OBJ_FLAG_HIDDEN),
                "T9.7c: overlay hidden after wake from OFF");

    display_sleep_deinit();
}

/* -------------------------------------------------------------------------
 * Group 11 — Panel cache attach/detach focus (Bug 2 regression)
 *
 * Phase A's LRU panel cache hides chat panels instead of destroying them
 * on switch-away (main.c.show_chat). Switch-back calls ui_chat_attach,
 * which restores module globals to the cached panel's widgets. The
 * regression: ui_chat_attach didn't focus the textarea, so after a Tab-
 * driven contact switch the LVGL group focus stayed on the sidebar row
 * that triggered the switch. Fix uses lv_async_call to defer the focus
 * past the FOCUSED handler chain so it sticks.
 * ---------------------------------------------------------------------- */

static lv_obj_t *make_panel(const char *name, const char *room_id)
{
    lv_obj_t *p = lv_obj_create(g_content_panel);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    ui_chat_create(p, name, room_id);
    return p;
}

/* lv_async_call is processed by lv_timer_handler. Pump a few ticks so
 * the deferred focus actually fires before we assert. */
static void pump_async(void)
{
    for (int i = 0; i < 4; i++) lv_timer_handler();
}

static void test_group11_panel_cache_focus(void)
{
    fprintf(stderr, "\n=== Group 11: Panel Cache Focus (Bug 2 regression) ===\n");

    reset_all();
    build_layout();
    lv_timer_handler();
    sync_indev();

    /* Build two cached panels. ui_chat_create on each should focus that
     * panel's textarea via the deferred lv_async_call. */
    lv_obj_t *panel_a = make_panel("Alice", "!alice:test");
    pump_async();
    lv_obj_t *ta_a = ui_chat_get_textarea();
    assert_focused(ta_a, "T11.1: textarea_a focused after ui_chat_create");

    /* Detach A and hide it (mirrors show_chat's switch-away path). */
    ui_chat_detach();
    lv_obj_add_flag(panel_a, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel_b = make_panel("Bob", "!bob:test");
    pump_async();
    lv_obj_t *ta_b = ui_chat_get_textarea();
    assert_focused(ta_b, "T11.2: textarea_b focused after second ui_chat_create");
    assert_true(ta_a != ta_b, "T11.2b: distinct textareas per panel");

    /* Cache-hit switch-back to A: detach B, hide B, unhide A, attach A.
     * The Bug 2 regression was that ui_chat_attach didn't focus → focus
     * stayed wherever it was. Now ui_chat_attach uses lv_async_call. */
    ui_chat_detach();
    lv_obj_add_flag(panel_b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel_a, LV_OBJ_FLAG_HIDDEN);
    /* Move focus elsewhere so the assertion is meaningful — without this
     * the test could pass just because ta_a was already focused. */
    lv_obj_t *row0 = ui_sidebar_get_row(0);
    if (row0) lv_group_focus_obj(row0);
    lv_timer_handler();
    assert_focused(row0, "T11.3a: focus moved to sidebar row before attach");

    ui_chat_attach(panel_a);
    pump_async();
    assert_focused(ta_a, "T11.3b: textarea_a focused after cache-hit ui_chat_attach");

    /* Repeat once more for B to confirm it isn't a one-shot. */
    ui_chat_detach();
    lv_obj_add_flag(panel_a, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel_b, LV_OBJ_FLAG_HIDDEN);
    if (row0) lv_group_focus_obj(row0);
    lv_timer_handler();
    ui_chat_attach(panel_b);
    pump_async();
    assert_focused(ta_b, "T11.4: textarea_b focused after second cache-hit attach");

    /* Cleanup: detach + free per-panel state, then delete the panels. */
    ui_chat_detach();
    ui_chat_free_panel_state(panel_a);
    ui_chat_free_panel_state(panel_b);
    lv_obj_delete(panel_a);
    lv_obj_delete(panel_b);
}

/* -------------------------------------------------------------------------
 * Group 32: per-contact typing language (lang.h)
 *
 * Keys go to the focused textarea as LV_EVENT_KEY with the character's
 * UTF-8 bytes packed into the key code, exactly as lv_xkb_process_key
 * delivers them on the Pi (and SDL for ASCII).
 * ---------------------------------------------------------------------- */

static void type_utf8(const char *s)
{
    while (*s) {
        size_t n = 1;
        while ((s[n] & 0xC0) == 0x80) n++;
        uint32_t key = 0;
        memcpy(&key, s, n);
        press_key(key);
        s += n;
    }
}

static void assert_textarea_is(const char *expected, const char *desc)
{
    lv_obj_t *ta = ui_chat_get_textarea();
    assert_str_eq(ta ? lv_textarea_get_text(ta) : "(no textarea)", expected, desc);
}

#ifdef CHATBOX_HAVE_SQLITE
static void snapshot(const char *name);   /* music section; needs lodepng */
#endif

/* Defined with the shell groups below. */
static void shell_setup(const char *boot);
static void shell_teardown(void);
static void avatars_setup(void);
static void avatars_teardown(void);

static void test_group32_typing_language(void)
{
    fprintf(stderr, "\n=== Group 32: Per-contact typing language ===\n");

    /* The Pi's textarea font: the emoji imgfont over font_chat_48 */
    if (!emoji_font_get_36()) emoji_font_init("pi/src/assets/emoji");
    if (!emoji_font_get_36()) emoji_font_init("/app/pi/src/assets/emoji");

    /* The real chat layout (app_chat.c), a contact open as show_chat leaves
     * it: the sidebar collapsed, Mom selected. */
    avatars_setup();
    shell_setup("chat");
    lv_timer_handler();
    ui_sidebar_set_selected(0);

    lv_obj_t *fr = make_panel("Mom", "!mom:test");
    ui_chat_set_avatar(ui_sidebar_avatar_src(0));
    ui_chat_set_language(LANG_FRENCH);
    pump_async();
    assert_not_null(emoji_font_get_36(), "T32.1: emoji font loaded (textarea uses it)");

    type_utf8("caf`e tr``es");
    assert_textarea_is("café très", "T32.2a: ` e -> é, `` e -> è (both ` deleted)");
    type_utf8(" `a ``a `u ``u ``E");
    assert_textarea_is("café très á à ú ù È", "T32.2b: same on every vowel, capitals too");
    type_utf8(" `c `");
    assert_textarea_is("café très á à ú ù È `c `", "T32.2c: ` before a consonant stays");
    press_key(LV_KEY_BACKSPACE);
    press_key(LV_KEY_BACKSPACE);
    assert_textarea_is("café très á à ú ù È `c", "T32.2d: Backspace");
    press_key(LV_KEY_BACKSPACE);
    type_utf8("u");
    assert_textarea_is("café très á à ú ù È ú", "T32.2e: combo after Backspace");
    press_key(LV_KEY_BACKSPACE);
    assert_textarea_is("café très á à ú ù È ", "T32.2f: Backspace removes the whole accented letter");
#ifdef CHATBOX_HAVE_SQLITE
    snapshot("chat_lang_french");
#endif

    /* Cursor moved: ` no longer right before the cursor */
    lv_textarea_set_text(ui_chat_get_textarea(), "`x");
    lv_textarea_set_cursor_pos(ui_chat_get_textarea(), LV_TEXTAREA_CURSOR_LAST);
    type_utf8("e");
    assert_textarea_is("`xe", "T32.3: no combo when ` isn't before the cursor");
    /* Cursor mid-text: the combo lands at the cursor, the rest is intact */
    lv_textarea_set_text(ui_chat_get_textarea(), "abcd");
    lv_textarea_set_cursor_pos(ui_chat_get_textarea(), 2);
    type_utf8("`e");
    assert_textarea_is("abécd", "T32.3b: ` e mid-text -> é at the cursor");
    type_utf8("``e");
    assert_textarea_is("abéècd", "T32.3c: `` e mid-text -> è at the cursor");
    lv_textarea_set_text(ui_chat_get_textarea(), "");

    /* Greek panel */
    ui_chat_detach();
    lv_obj_add_flag(fr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *gr = make_panel("Dad", "!dad:test");
    ui_sidebar_set_selected(1);
    ui_chat_set_avatar(ui_sidebar_avatar_src(1));
    ui_chat_set_language(LANG_GREEK);
    pump_async();
    type_utf8("καλημ`ερ`α `Ω ηγ gyp 😀");
    assert_textarea_is("καλημέρά Ώ ηγ gyp 😀", "T32.4b: Greek tonos");
    lv_textarea_set_text(ui_chat_get_textarea(), "");
    type_utf8("``α");
    assert_textarea_is("`ά", "T32.4g: Greek `` + vowel keeps the first `");
#ifdef CHATBOX_HAVE_SQLITE
    snapshot("chat_lang_greek");
#endif
    {
        /* Descenders fit: the textarea font's line box holds the fallback's
         * full line height (it used to be the 48 px emoji's, clipping ηγ gyp) */
        const lv_font_t *f = lv_obj_get_style_text_font(ui_chat_get_textarea(), 0);
        assert_true(lv_font_get_line_height(f) >= font_chat_48.line_height,
                    "T32.4c: textarea line height fits font_chat_48 (descenders not clipped)");
        /* ...and the line fits the 80 px box, so typing never scrolls it
         * vertically (it used to bounce by a few px) */
        lv_obj_t *ta = ui_chat_get_textarea();
        lv_obj_update_layout(ta);
        assert_true(lv_obj_get_content_height(ta) >= lv_font_get_line_height(f),
                    "T32.4e: textarea content height holds one line");
        assert_true(lv_obj_get_scroll_y(ta) == 0 && lv_obj_get_scroll_bottom(ta) <= 0,
                    "T32.4f: textarea has no vertical scroll");
    }
    lv_textarea_set_text(ui_chat_get_textarea(), "");
    type_utf8("`e");
    assert_textarea_is("`e", "T32.4d: Latin e not accented in Greek");

    /* Back to Mom: language is per panel */
    ui_chat_detach();
    lv_obj_add_flag(gr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fr, LV_OBJ_FLAG_HIDDEN);
    ui_chat_attach(fr);
    pump_async();
    lv_textarea_set_text(ui_chat_get_textarea(), "");
    type_utf8("`a");
    assert_textarea_is("á", "T32.5b: French combo after attach");

    /* LANG_NONE: no combo */
    ui_chat_set_language(LANG_NONE);
    lv_textarea_set_text(ui_chat_get_textarea(), "");
    type_utf8("`a");
    assert_textarea_is("`a", "T32.6b: no combo without a language");

    ui_chat_detach();
    ui_chat_free_panel_state(fr);
    ui_chat_free_panel_state(gr);
    lv_obj_delete(fr);
    lv_obj_delete(gr);
    shell_teardown();
    avatars_teardown();
}

/* =========================================================================
 * App shell groups (T4, docs/player-spec.md §5, §6, §8)
 *
 * These drive keys through the virtual keypad indev — the same
 * lv_indev_read → read_cb path the Pi and SDL drivers take — with the shell's
 * read hook installed on it, and a fake clock so LVGL's held-key repeats
 * (400 ms, then every 200 ms) and the 450 ms one-shot window are exact.
 * ====================================================================== */

static bool g_shell_up     = false;
static bool g_shell_hooked = false;

/* Second/third keyboard for the hotplug group. */
static uint32_t         g_pending2_key   = 0;
static lv_indev_state_t g_pending2_state = LV_INDEV_STATE_RELEASED;

static void test_keypad2_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->key = g_pending2_key;
    data->state = g_pending2_state;
}

static lv_group_t *chat_grp(void)  { return app_chat.group(); }
static lv_group_t *music_grp(void) { return app_music.group(); }
static lv_obj_t   *chat_root(void) { return lv_obj_get_parent(app_chat_sidebar_panel()); }

/* Advance the fake clock in 4 ms steps, running LVGL each step, so the
 * indev read timer (16 ms) and the animation timer fire as in the main loop. */
static void clock_advance(uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = ms < 4 ? ms : 4;
        g_fake_now += step;
        ms -= step;
        lv_timer_handler();
    }
}

static void key_down(uint32_t key)
{
    g_pending_key = key;
    g_pending_state = LV_INDEV_STATE_PRESSED;
    force_indev_read();
}

static void key_up(void)
{
    g_pending_state = LV_INDEV_STATE_RELEASED;
    force_indev_read();
}

/* Press + release, then `gap` ms. */
static void tap_gap(uint32_t key, uint32_t gap)
{
    key_down(key);
    key_up();
    clock_advance(gap);
}

/* A deliberate tap: 500 ms of quiet after it, so the next press of any key
 * is outside the 450 ms window and every animation has finished. */
static void tap(uint32_t key)
{
    tap_gap(key, 500);
}

/* Pi hold: one press held for `ms` (LVGL repeats non-Enter keys itself). */
static void hold_pi(uint32_t key, uint32_t ms)
{
    key_down(key);
    clock_advance(ms);
    key_up();
    clock_advance(20);
}

/* SDL keyboard (§8.1 release reset). LVGL's SDL driver turns every
 * SDL_KEYDOWN, OS repeats included, into a press + an immediate fake
 * release, so the shell takes real key-ups from main.c's event watch
 * instead. sdl_mode(true) marks the test indev that way; the helpers below
 * call the shell exactly as the watch does. */
static void sdl_mode(bool on)
{
    shell_keyboard_use_external_release(g_test_indev, on);
}

/* One SDL_KEYDOWN as LVGL delivers it: the watch saw it queued, then the
 * driver's press + fake release; then `gap` ms. */
static void sdl_down_gap(uint32_t key, uint32_t gap)
{
    shell_key_pushed(key);
    key_down(key);
    key_up();
    clock_advance(gap);
}

/* The real SDL_KEYUP (the watch). */
static void sdl_keyup(uint32_t key)
{
    shell_key_released(key);
}

/* A separate SDL tap: key-down, key-up, then `gap` ms. */
static void sdl_tap_gap(uint32_t key, uint32_t gap)
{
    sdl_mode(true);
    shell_key_pushed(key);
    key_down(key);
    key_up();
    sdl_keyup(key);
    clock_advance(gap);
    sdl_mode(false);
}

/* SDL hold: the first press, then after a 300 ms delay an OS repeat every
 * 33 ms, each a press + fake release with no key-up in between; the real
 * key-up at the end. */
static void hold_sdl(uint32_t key, uint32_t ms)
{
    sdl_mode(true);
    sdl_down_gap(key, 300);
    for (uint32_t t = 300; t < ms; t += 33) sdl_down_gap(key, 33);
    sdl_keyup(key);
    clock_advance(20);
    sdl_mode(false);
}

static bool obj_under(lv_obj_t *obj, lv_obj_t *root)
{
    for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o)) {
        if (o == root) return true;
    }
    return false;
}

static void shell_teardown(void)
{
    if (!g_shell_up) return;
    g_current_contact = -1;
    ui_chat_destroy();
    shell_deinit();
    g_shell_up = false;
    reset_all();
}

/* Group 34 sets this: the shell gets the Photos app too (three tiles). */
static bool g_with_photos = false;

/* Fresh shell, like main.c's build_shell: Music tile left, Chat right (and
 * Photos third with g_with_photos). */
static void shell_setup(const char *boot)
{
    shell_teardown();
    reset_all();
    if (!g_fake_clock) {
        g_fake_now = test_tick_cb();
        g_fake_clock = true;
    }
    if (!g_shell_hooked) {
        shell_init();
        shell_attach_keyboard(g_test_indev);
        g_shell_hooked = true;
    }
    g_pending_key = 0;
    g_pending_state = LV_INDEV_STATE_RELEASED;
    lv_indev_reset(g_test_indev, NULL);

    /* The screen as main.c leaves it: build_layout (groups 1-11) made it a
     * flex row, which would lay the switcher out beside the visible app. */
    lv_obj_set_layout(lv_screen_active(), LV_LAYOUT_NONE);

    app_chat_configure(&g_cfg, on_contact_selected);
    shell_register_app(&app_music, true);
    shell_register_app(&app_chat, false);
    if (g_with_photos) shell_register_app(&app_photos, true);
    shell_start(boot);
    g_shell_up = true;
    g_sidebar_panel = app_chat_sidebar_panel();
    g_content_panel = app_chat_content_panel();
    app_music_test_set_ctx(KEY_CTX_NO_MUSIC);
    clock_advance(500);
    app_music_test_reset_counters();
}

/* Open contact 0 with Enter on its sidebar row (through the indev). */
static void open_contact0(void)
{
    lv_group_focus_obj(ui_sidebar_get_row(0));
    tap(LV_KEY_ENTER);
}

static void test_group12_shell_boot_and_esc(void)
{
    fprintf(stderr, "\n=== Group 12: Shell boot + Esc in chat ===\n");
    shell_setup("chat");

    assert_true(shell_visible_app() == &app_chat, "T12.1a: default_app = chat boots into chat");
    assert_true(lv_indev_get_group(g_test_indev) == chat_grp(), "T12.1b: keyboard on chat's group");
    assert_true(lv_group_get_default() == chat_grp(), "T12.1c: chat's group stays LVGL's default");
    assert_visible(chat_root(), "T12.1d: chat root visible");
    assert_hidden(app_music_get_root(), "T12.1e: music root hidden");
    assert_hidden(switcher_get_root(), "T12.1f: switcher hidden");

    lv_obj_t *row0 = ui_sidebar_get_row(0);
    assert_focused(row0, "T12.2a: chat focus on row[0] at boot");
    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T12.2b: Esc in chat (no emoji panel) opens the switcher");
    assert_visible(switcher_get_root(), "T12.2c: switcher overlay visible");
    assert_true(switcher_get_focus() == 1, "T12.2d: focus starts on the Chat tile");
    assert_true(switcher_get_current() == 1, "T12.2e: Chat is the current tile");
    assert_visible(switcher_get_now_tag(1), "T12.2f: NOW tag on the Chat tile");
    assert_hidden(switcher_get_now_tag(0), "T12.2g: no NOW tag on the Music tile");
    assert_true(lv_indev_get_group(g_test_indev) == switcher_group(),
                "T12.2h: keyboard on the switcher's group");

    tap(LV_KEY_ESC);
    assert_true(!shell_switcher_is_open(), "T12.3a: Esc in the switcher closes it");
    assert_hidden(switcher_get_root(), "T12.3b: switcher overlay hidden");
    assert_true(shell_visible_app() == &app_chat, "T12.3c: back to chat");
    assert_true(lv_indev_get_group(g_test_indev) == chat_grp(), "T12.3d: keyboard back on chat's group");
    assert_focused(row0, "T12.3e: chat focus unchanged");
}

static void test_group13_esc_layering_emoji(void)
{
    fprintf(stderr, "\n=== Group 13: Esc layering (emoji panel) ===\n");
    shell_setup("chat");
    open_contact0();

    lv_obj_t *ta = ui_chat_get_textarea();
    assert_not_null(ta, "T13.1a: Enter on row[0] opened the chat");
    assert_focused(ta, "T13.1b: text box focused");
    lv_textarea_add_text(ta, "hello");

    tap(LV_KEY_MENU);
    assert_not_null(ui_chat_get_emoji_panel(), "T13.2a: Win opens the emoji panel (unchanged)");
    tap(LV_KEY_PG_DOWN);
    assert_size_eq(ui_chat_get_emoji_page_idx(), 1, "T13.2b: PgDn pages the emoji panel (unchanged)");
    tap(LV_KEY_PG_UP);
    assert_size_eq(ui_chat_get_emoji_page_idx(), 0, "T13.2c: PgUp pages back (unchanged)");

    tap(LV_KEY_ESC);
    assert_null(ui_chat_get_emoji_panel(), "T13.3a: Esc with the panel open closes the panel");
    assert_true(!shell_switcher_is_open(), "T13.3b: ...and only the panel (no switcher)");
    assert_focused(ta, "T13.3c: focus back on the text box");
    assert_textarea_contains("hello", "T13.3d: typed text kept");

    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T13.4a: next Esc (panel closed) opens the switcher");
    assert_true(switcher_get_focus() == 1, "T13.4b: focused on the Chat tile");
    tap(LV_KEY_ESC);
    assert_true(!shell_switcher_is_open(), "T13.4c: Esc closes it again");
    assert_focused(ta, "T13.4d: text box still focused");
}

static void test_group14_switcher_keys(void)
{
    fprintf(stderr, "\n=== Group 14: Switcher keys (§8.5) + app switching ===\n");
    shell_setup("chat");
    open_contact0();
    lv_obj_t *ta = ui_chat_get_textarea();
    lv_textarea_add_text(ta, "hello");

    /* Enough bubbles to scroll, then scroll up a bit. */
    lv_obj_t *msgs = ui_chat_get_msg_cont();
    for (int i = 0; i < 30; i++) ui_chat_add_message("scroll me", i % 2);
    clock_advance(600);
    lv_obj_scroll_to_y(msgs, 300, LV_ANIM_OFF);
    clock_advance(50);
    int32_t scroll = lv_obj_get_scroll_y(msgs);
    assert_true(scroll > 0, "T14.0: chat scrolled (precondition)");

    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 1,
                "T14.1: switcher open on the Chat tile");
    int shakes = switcher_get_shake_count();
    int bounces = switcher_get_bounce_count();

    tap(LV_KEY_RIGHT);
    assert_true(switcher_get_focus() == 1, "T14.2a: -> on Chat (right edge) keeps focus");
    assert_true(switcher_get_shake_count() == shakes + 1, "T14.2b: -> on Chat shakes");
    tap(LV_KEY_LEFT);
    assert_true(switcher_get_focus() == 0, "T14.3a: <- focuses Music");
    tap(LV_KEY_LEFT);
    assert_true(switcher_get_focus() == 0, "T14.3b: <- on Music (left edge) keeps focus");
    assert_true(switcher_get_shake_count() == shakes + 2, "T14.3c: <- on Music shakes");
    tap(LV_KEY_RIGHT);
    assert_true(switcher_get_focus() == 1, "T14.3d: -> focuses Chat");
    tap(LV_KEY_UP);
    assert_true(switcher_get_bounce_count() == bounces + 1, "T14.4a: Up bounces");
    tap(LV_KEY_DOWN);
    assert_true(switcher_get_bounce_count() == bounces + 2, "T14.4b: Down bounces");
    assert_true(switcher_get_focus() == 1 && shell_switcher_is_open(),
                "T14.4c: Up/Down move nothing");

    /* Keys the switcher ignores (§8.5), incl. the acceptance trio. */
    shakes = switcher_get_shake_count();
    bounces = switcher_get_bounce_count();
    tap(LV_KEY_MENU);
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 1 &&
                ui_chat_get_emoji_panel() == NULL, "T14.5a: Win in the switcher does nothing");
    tap(' ');
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 1,
                "T14.5b: Space in the switcher does nothing");
    tap(LV_KEY_PG_UP);
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 1 &&
                ui_chat_get_emoji_page_idx() == 0, "T14.5c: PgUp in the switcher does nothing");
    tap(LV_KEY_PG_DOWN);
    tap('a');
    tap(LV_KEY_BACKSPACE);
    tap(LV_KEY_NEXT);
    tap(LV_KEY_PREV);
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 1 &&
                switcher_get_shake_count() == shakes &&
                switcher_get_bounce_count() == bounces,
                "T14.5d: PgDn, letters, Backspace, Tab do nothing either");
    assert_true(strcmp(lv_textarea_get_text(ta), "hello") == 0,
                "T14.5e: no key typed into chat behind the switcher");
    assert_true(shell_visible_app() == &app_chat, "T14.5f: chat still the current app");

    tap(LV_KEY_ENTER);
    assert_true(!shell_switcher_is_open() && !switcher_is_zooming(),
                "T14.6a: Enter on the current (Chat) tile just closes");
    assert_true(shell_visible_app() == &app_chat, "T14.6b: chat still visible");

    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    tap_gap(LV_KEY_ENTER, 20);
    assert_true(switcher_is_zooming(), "T14.7a: Enter on Music zooms the tile");
    clock_advance(480);
    assert_true(shell_visible_app() == &app_music, "T14.7b: ...then music is visible");
    assert_true(!shell_switcher_is_open(), "T14.7c: switcher closed");
    assert_visible(app_music_get_root(), "T14.7d: music root shown");
    assert_hidden(chat_root(), "T14.7e: chat root hidden");
    assert_true(lv_indev_get_group(g_test_indev) == music_grp(), "T14.7f: keyboard on music's group");

    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T14.8a: Esc from music opens the switcher");
    assert_true(switcher_get_focus() == 0, "T14.8b: focused on the Music tile");
    assert_true(switcher_get_current() == 0, "T14.8c: Music is the current tile");
    assert_visible(switcher_get_now_tag(0), "T14.8d: NOW tag on Music");

    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    assert_true(shell_visible_app() == &app_chat, "T14.9a: Enter on the Chat tile shows chat");
    assert_true(lv_indev_get_group(g_test_indev) == chat_grp(), "T14.9b: keyboard on chat's group");
    assert_focused(ta, "T14.9c: chat focus kept (text box)");
    assert_textarea_contains("hello", "T14.9d: typed text kept");
    assert_true(g_current_contact == 0 && ui_chat_get_textarea() == ta,
                "T14.9e: selected contact kept");
    assert_true(lv_obj_get_scroll_y(msgs) == scroll, "T14.9f: scroll position kept");
}

static void test_group15_held_keys(void)
{
    fprintf(stderr, "\n=== Group 15: Held keys and the one-shot rule ===\n");
    shell_setup("chat");

    hold_pi(LV_KEY_ESC, 1500);
    assert_true(shell_switcher_is_open(), "T15.1a: held Esc (Pi, 1.5 s) opens the switcher once");
    clock_advance(500);
    hold_pi(LV_KEY_ESC, 1500);
    assert_true(!shell_switcher_is_open(), "T15.1b: held Esc again closes it once");
    clock_advance(500);
    hold_sdl(LV_KEY_ESC, 1500);
    assert_true(shell_switcher_is_open(), "T15.1c: held Esc (SDL repeats) toggles once");
    clock_advance(500);
    hold_sdl(LV_KEY_ESC, 1500);
    assert_true(!shell_switcher_is_open(), "T15.1d: ...and once again");
    clock_advance(500);

    /* ↑/↓ repeat in the switcher: proves LVGL's repeats reach it. */
    tap(LV_KEY_ESC);
    int bounces = switcher_get_bounce_count();
    hold_pi(LV_KEY_UP, 1500);
    assert_true(switcher_get_bounce_count() - bounces >= 4,
                "T15.2: held Up bounces repeatedly (LVGL repeat reaches the switcher)");
    clock_advance(500);

    /* → is one-shot: the same repeats are dropped. */
    tap(LV_KEY_LEFT);
    int shakes = switcher_get_shake_count();
    hold_pi(LV_KEY_RIGHT, 1500);
    assert_true(switcher_get_focus() == 1, "T15.3a: held -> (Pi) moves to Chat once");
    assert_true(switcher_get_shake_count() == shakes,
                "T15.3b: its LVGL repeats are dropped (no edge shake)");
    clock_advance(500);
    hold_sdl(LV_KEY_LEFT, 1500);
    assert_true(switcher_get_focus() == 0 && switcher_get_shake_count() == shakes,
                "T15.3c: held <- (SDL) moves once, repeats dropped");
    clock_advance(500);

    /* Release reset (§8.1): separate presses all act, however fast.
     * (Before, the second -> 100 ms later was dropped.) */
    tap_gap(LV_KEY_RIGHT, 100);
    tap_gap(LV_KEY_RIGHT, 500);
    assert_true(switcher_get_focus() == 1 && switcher_get_shake_count() == shakes + 1,
                "T15.4a: second -> 100 ms later (released in between) acts: edge shake");
    tap(LV_KEY_RIGHT);
    assert_true(switcher_get_shake_count() == shakes + 2,
                "T15.4b: -> 500 ms after the last one acts too");

    /* Held Enter on the Music tile (Pi: one press, never repeated). */
    tap(LV_KEY_LEFT);
    app_music_test_reset_counters();
    hold_pi(LV_KEY_ENTER, 1500);
    assert_true(shell_visible_app() == &app_music, "T15.5a: held Enter on Music opens music");
    assert_true(app_music_get_key_events() == 0,
                "T15.5b: nothing of that press reaches music (no KEY/CLICKED)");
    clock_advance(500);

    /* Same with SDL repeats, music filtering as the columns view (Enter
     * one-shot): the repeats reach music but the global record drops them. */
    tap(LV_KEY_ESC);
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    assert_true(shell_visible_app() == &app_chat && switcher_get_focus() == 0,
                "T15.6a: back in chat, switcher on the Music tile");
    app_music_test_set_ctx(KEY_CTX_MUSIC_BROWSE);
    app_music_test_reset_counters();
    hold_sdl(LV_KEY_ENTER, 1500);
    assert_true(shell_visible_app() == &app_music, "T15.6b: held Enter (SDL) opens music");
    assert_true(app_music_get_key_events() > 0, "T15.6c: the repeats reach music's group");
    assert_true(app_music_get_accepted(LV_KEY_ENTER) == 0,
                "T15.6d: ...and are all dropped (Enter doesn't then act in music)");
    clock_advance(500);
    tap(LV_KEY_ENTER);
    assert_true(app_music_get_accepted(LV_KEY_ENTER) == 1,
                "T15.6e: a fresh Enter 500 ms later is accepted");
    app_music_test_set_ctx(KEY_CTX_NO_MUSIC);
}

static void test_group16_anim_interrupt(void)
{
    fprintf(stderr, "\n=== Group 16: Keys during switcher animations ===\n");
    shell_setup("chat");
    open_contact0();
    lv_obj_t *ta = ui_chat_get_textarea();

    tap_gap(LV_KEY_ESC, 20);
    assert_true(shell_switcher_is_open() && switcher_is_animating(),
                "T16.1a: fly-in running 20 ms after Esc");
    tap_gap(LV_KEY_LEFT, 0);
    assert_true(!switcher_is_animating(), "T16.1b: a key jumps the fly-in to its end");
    assert_true(lv_obj_get_style_translate_x(switcher_get_tile(1), 0) == 0,
                "T16.1c: tiles at rest");
    assert_true(switcher_get_focus() == 0, "T16.1d: ...and the key is then handled (<- to Music)");
    clock_advance(500);

    app_music_test_reset_counters();
    tap_gap(LV_KEY_ENTER, 20);
    assert_true(switcher_is_zooming(), "T16.2a: zoom running 20 ms after Enter");
    tap_gap(' ', 0);
    assert_true(!switcher_is_zooming() && shell_visible_app() == &app_music &&
                !shell_switcher_is_open(), "T16.2b: a key completes the zoom (music shown)");
    assert_true(app_music_get_accepted(' ') == 1,
                "T16.2c: ...and lands in music, through its filter");
    clock_advance(500);

    tap(LV_KEY_ESC);
    tap(LV_KEY_RIGHT);
    tap_gap(LV_KEY_ENTER, 20);
    assert_true(switcher_is_zooming(), "T16.3a: zoom to chat running");
    tap_gap('x', 0);
    assert_true(shell_visible_app() == &app_chat, "T16.3b: typing completes the zoom (chat shown)");
    assert_true(ui_chat_get_textarea() == ta, "T16.3c: same chat");
    assert_textarea_contains("x", "T16.3d: the typed key lands in the text box");
    clock_advance(500);
}

static void test_group17_focus_isolation(void)
{
    fprintf(stderr, "\n=== Group 17: Focus isolation (D3) ===\n");
    shell_setup("chat");

    bool foreign = false;
    uint32_t n = lv_group_get_obj_count(chat_grp());
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_group_get_obj_by_index(chat_grp(), i);
        if (obj_under(o, app_music_get_root()) || obj_under(o, switcher_get_root()))
            foreign = true;
    }
    assert_true(n > 0 && !foreign, "T17.1: chat's group holds only chat objects");
    assert_true(lv_group_get_obj_count(music_grp()) == 1, "T17.2a: music's group is one key sink");
    assert_true(lv_group_get_obj_count(switcher_group()) == 1, "T17.2b: switcher's group is one key sink");

    open_contact0();
    bool leaked = false;
    for (int i = 0; i < 8; i++) {
        tap(LV_KEY_NEXT);
        lv_obj_t *f = lv_group_get_focused(lv_indev_get_group(g_test_indev));
        if (!f || lv_obj_get_group(f) != chat_grp() ||
            obj_under(f, app_music_get_root()) || obj_under(f, switcher_get_root()))
            leaked = true;
    }
    assert_true(!leaked, "T17.3: Tab in chat never focuses another app's object");

    lv_obj_t *ta = ui_chat_get_textarea();
    lv_group_focus_obj(ta);
    clock_advance(50);
    tap(LV_KEY_ESC);
    lv_obj_t *sink = lv_group_get_obj_by_index(switcher_group(), 0);
    for (int i = 0; i < 3; i++) tap(LV_KEY_NEXT);
    assert_true(lv_group_get_focused(switcher_group()) == sink,
                "T17.4a: Tab in the switcher stays on its sink");
    assert_true(lv_group_get_focused(chat_grp()) == ta, "T17.4b: chat's focus untouched");
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 1,
                "T17.4c: switcher unchanged by Tab");

    tap(LV_KEY_LEFT);
    tap(LV_KEY_ENTER);
    for (int i = 0; i < 3; i++) tap(LV_KEY_NEXT);
    assert_true(shell_visible_app() == &app_music &&
                lv_group_get_focused(music_grp()) == app_music_get_sink(),
                "T17.5a: Tab in music stays on its sink");
    assert_true(lv_group_get_focused(chat_grp()) == ta, "T17.5b: chat's focus untouched");
}

static void test_group18_keyboard_hotplug(void)
{
    fprintf(stderr, "\n=== Group 18: Keyboard hotplug re-attach ===\n");
    shell_setup("chat");
    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    tap(LV_KEY_ENTER);
    assert_true(shell_visible_app() == &app_music, "T18.1a: music visible");

    /* main.c attach_keyboard(): a new libinput indev + shell_attach_keyboard. */
    lv_indev_t *kb2 = lv_indev_create();
    lv_indev_set_type(kb2, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb2, test_keypad2_read_cb);
    shell_attach_keyboard(kb2);
    assert_true(lv_indev_get_group(kb2) == music_grp(),
                "T18.1b: keyboard attached while music is up uses music's group");

    g_pending2_key = LV_KEY_ESC;
    g_pending2_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(kb2);
    g_pending2_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(kb2);
    clock_advance(500);
    assert_true(shell_switcher_is_open(), "T18.2a: its Esc goes through the shell (switcher opens)");
    assert_true(lv_indev_get_group(kb2) == switcher_group(), "T18.2b: it follows to the switcher's group");

    /* Unplug + replug while the switcher is open. */
    lv_indev_delete(kb2);
    lv_indev_t *kb3 = lv_indev_create();
    lv_indev_set_type(kb3, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb3, test_keypad2_read_cb);
    shell_attach_keyboard(kb3);
    assert_true(lv_indev_get_group(kb3) == switcher_group(),
                "T18.3a: keyboard re-attached while the switcher is up uses its group");
    g_pending2_key = LV_KEY_ESC;
    g_pending2_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(kb3);
    g_pending2_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(kb3);
    clock_advance(500);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music,
                "T18.3b: the new keyboard's Esc closes it (hooked), music still up");
    assert_true(lv_indev_get_group(kb3) == music_grp(), "T18.3c: back on music's group");
    lv_indev_delete(kb3);
}

static void test_group19_lock_with_shell(void)
{
    fprintf(stderr, "\n=== Group 19: Display lock with the shell ===\n");
    shell_setup("chat");
    display_sleep_init(1, 2);

    /* Locked while music is up. */
    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    tap(LV_KEY_ENTER);
    clock_advance(1100);
    display_sleep_force_check();
    lv_obj_t *ov = display_sleep_get_overlay();
    assert_true(display_sleep_get_state() == 1, "T19.1a: LOCKED while music is up");
    assert_true(lv_obj_get_group(ov) == music_grp() &&
                lv_group_get_focused(music_grp()) == ov,
                "T19.1b: overlay joined and focused in music's group");
    app_music_test_reset_counters();
    tap_gap(LV_KEY_ESC, 50);
    assert_true(!shell_switcher_is_open(), "T19.1c: Esc while locked doesn't open the switcher");
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 0, "T19.1d: ...it only wakes the display");
    assert_true(shell_visible_app() == &app_music && !shell_switcher_is_open(),
                "T19.1e: still music, no switcher");
    assert_true(lv_group_get_focused(music_grp()) == app_music_get_sink() &&
                lv_obj_get_group(ov) == NULL, "T19.1f: focus back on music's sink, overlay out");
    assert_true(app_music_get_key_events() == 0, "T19.1g: nothing reached music");
    /* Release reset (§8.1): the wake Esc was released, so the next Esc acts
     * however soon. (Before, an Esc 50 ms after the wake was dropped.) */
    tap_gap(LV_KEY_ESC, 500);
    assert_true(shell_switcher_is_open(),
                "T19.1h: wake Esc released: an Esc 50 ms later opens the switcher");
    assert_true(switcher_get_focus() == 0 && switcher_get_current() == 0,
                "T19.1i: ...once, on the Music tile");

    /* Locked while the switcher is open; a held Esc wakes and does nothing more. */
    clock_advance(1100);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1 && lv_obj_get_group(ov) == switcher_group(),
                "T19.2a: LOCKED with the switcher up: overlay in the switcher's group");
    key_down(LV_KEY_ESC);
    clock_advance(20);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 0, "T19.2b: held Esc wakes the display");
    clock_advance(1000);
    key_up();
    clock_advance(500);
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 0,
                "T19.2c: switcher still open and unchanged after the held Esc");
    assert_true(lv_group_get_focused(switcher_group()) ==
                lv_group_get_obj_by_index(switcher_group(), 0),
                "T19.2d: focus back on the switcher's sink");
    assert_true(lv_indev_get_group(g_test_indev) == switcher_group(),
                "T19.2e: keyboard still on the switcher's group");

    /* Locked in chat with the text box focused. */
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    open_contact0();
    lv_obj_t *ta = ui_chat_get_textarea();
    lv_textarea_add_text(ta, "abc");
    clock_advance(1100);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1 && lv_obj_get_group(ov) == chat_grp(),
                "T19.3a: LOCKED in chat: overlay in chat's group");
    tap_gap(LV_KEY_ENTER, 50);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 0, "T19.3b: Enter wakes");
    assert_focused(ta, "T19.3c: focus back on the text box");
    assert_true(g_current_contact == 0, "T19.3d: same contact");
    assert_textarea_contains("abc", "T19.3e: the wake Enter didn't send");

    /* SDL keyboard: Esc held down to wake (OS repeats, no key-up until the
     * end) wakes only; once released, the next Esc acts at once. */
    clock_advance(1100);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1, "T19.4a: LOCKED in chat again");
    sdl_mode(true);
    sdl_down_gap(LV_KEY_ESC, 20);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 0, "T19.4b: SDL Esc press wakes");
    clock_advance(280);
    for (int i = 0; i < 30; i++) sdl_down_gap(LV_KEY_ESC, 33);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_chat,
                "T19.4c: its OS repeats after the wake do nothing (switcher stays shut)");
    assert_textarea_contains("abc", "T19.4d: chat untouched");
    sdl_keyup(LV_KEY_ESC);
    clock_advance(30);
    sdl_down_gap(LV_KEY_ESC, 0);
    sdl_keyup(LV_KEY_ESC);
    clock_advance(500);
    sdl_mode(false);
    assert_true(shell_switcher_is_open(),
                "T19.4e: after the real key-up, an Esc 30 ms later opens the switcher");
    tap(LV_KEY_ESC);

    display_sleep_deinit();
}

static void test_group20_boot_music(void)
{
    fprintf(stderr, "\n=== Group 20: default_app = music ===\n");
    config_t cfg = g_cfg;
    cfg.default_app = CONFIG_APP_MUSIC;
    shell_setup(app_id_for_config(cfg.default_app));

    assert_true(shell_visible_app() == &app_music, "T20.1a: default_app = music boots into music");
    assert_visible(app_music_get_root(), "T20.1b: music root visible");
    assert_hidden(chat_root(), "T20.1c: chat root hidden");
    assert_true(lv_indev_get_group(g_test_indev) == music_grp(), "T20.1d: keyboard on music's group");
    assert_true(lv_group_get_default() == chat_grp(), "T20.1e: chat's group still LVGL's default");

    tap(' ');
    tap(LV_KEY_PG_UP);
    tap(LV_KEY_PG_DOWN);
    tap(LV_KEY_ENTER);
    tap(LV_KEY_MENU);
    assert_true(shell_visible_app() == &app_music && !shell_switcher_is_open(),
                "T20.2a: Space/PgUp/PgDn/Enter/Win in music: no-ops, music stays");
    assert_true(app_music_get_accepted(' ') == 1 && app_music_get_accepted(LV_KEY_PG_UP) == 1,
                "T20.2b: they are routed to music (T8 fills them in)");

    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open() && switcher_get_focus() == 0 &&
                switcher_get_current() == 0, "T20.3a: Esc opens the switcher on Music");
    tap(LV_KEY_ESC);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music,
                "T20.3b: Esc closes it, music unchanged");

    /* default_app = home: the switcher over chat, as main.c boots it. */
    cfg.default_app = CONFIG_APP_HOME;
    shell_setup(app_id_for_config(cfg.default_app));
    if (app_boot_home(cfg.default_app)) shell_open_switcher();
    clock_advance(500);
    assert_true(shell_switcher_is_open() && shell_visible_app() == &app_chat &&
                switcher_get_focus() == 1,
                "T20.4a: default_app = home boots into the switcher, on Chat");
#ifdef CHATBOX_KEY_HINTS
    {
        int n = 0, sn = 0;
        lv_obj_t *scr = lv_screen_active();
        lv_obj_t *top = lv_obj_get_child(scr, (int32_t)lv_obj_get_child_count(scr) - 1);
        assert_true(top == ui_hints_bar() && ui_hints_shown(&n) == switcher_hints(&sn),
                    "T20.4h: the hint bar is on top of the switcher, with its hints");
    }
#endif
    tap(LV_KEY_LEFT);
    tap(LV_KEY_ENTER);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music,
                "T20.4b: Left, Enter opens music");
}

/* =========================================================================
 * Music app (T8): docs/player-spec.md §7, §8.2–§8.4.
 *
 * The real music UI (ui_music.c + app_music.c) through the real shell/key
 * path (test indev → shell read hook → music's key sink → key filter),
 * against:
 *   - a real library DB built by the indexer's scan with the stub prober
 *     (test_music_fixture.c), read through library.c;
 *   - a fake player (music_player_ops_t) that models player.h's semantics
 *     and fires its callbacks only from its own tick (an lv_timer here, like
 *     player_tick in main.c).
 * ====================================================================== */

#ifdef CHATBOX_HAVE_SQLITE

#include "indexer_scan.h"

#include <sys/stat.h>

/* ---- Fake player ---------------------------------------------------------- */

#define FP_MAX 1024

static struct {
    int64_t        ids[FP_MAX];
    char           path0[PATH_MAX];
    int            n, idx;
    player_state_t st;
    int64_t        pos, dur;
    int            vol, vmax;
    player_callbacks_t cb;
    bool           d_track, d_state, d_pos, d_vol;
    /* call counters */
    int            c_queue, c_toggle, c_restart, c_prev, c_next, c_vol;
    int            last_n, last_start;
} fp;

static lv_timer_t *g_fp_timer = NULL;

static void fp_reset(void)
{
    memset(&fp, 0, sizeof(fp));
    fp.idx  = -1;
    fp.st   = PLAYER_IDLE;
    fp.dur  = -1;
    fp.vol  = 40;
    fp.vmax = 70;
}

static void fp_set_callbacks(const player_callbacks_t *cb) { fp.cb = *cb; }

static void fp_load(int idx, bool play)
{
    bool changed = (idx != fp.idx);
    fp.idx = idx;
    fp.pos = 0;
    fp.dur = -1;                    /* unknown until "mpv" says: the UI uses the library's */
    fp.st  = play ? PLAYER_PLAYING : PLAYER_PAUSED;
    fp.d_track |= changed;
    fp.d_state = fp.d_pos = true;
}

static bool fp_play_queue(const player_track_t *t, int n, int start)
{
    fp.c_queue++;
    fp.last_n = n;
    fp.last_start = start;
    if (n <= 0) return false;
    if (n > FP_MAX) n = FP_MAX;
    for (int i = 0; i < n; i++) fp.ids[i] = t[i].id;
    snprintf(fp.path0, sizeof(fp.path0), "%s", t[0].path);
    fp.n = n;
    fp.idx = -1;
    fp_load(start < 0 ? 0 : start >= n ? n - 1 : start, true);
    fp.d_track = true;
    return true;
}

/* Space: playing ↔ paused; stopped / idle: nothing (no replay). */
static void fp_toggle(void)
{
    fp.c_toggle++;
    switch (fp.st) {
    case PLAYER_IDLE:
    case PLAYER_STOPPED: return;
    case PLAYER_PLAYING: fp.st = PLAYER_PAUSED; break;
    case PLAYER_PAUSED:  fp.st = PLAYER_PLAYING; break;
    }
    fp.d_state = true;
}

/* Enter on Now Playing: the current track from 0:00, playing. */
static void fp_restart(void)
{
    fp.c_restart++;
    if (fp.st == PLAYER_IDLE) return;
    fp.pos = 0;
    fp.st = PLAYER_PLAYING;
    fp.d_state = fp.d_pos = true;
}

/* iPod rule: > 3 s in or on the first track → restart; else previous. */
static void fp_prev(void)
{
    fp.c_prev++;
    if (fp.st == PLAYER_IDLE) return;
    bool play = fp.st == PLAYER_PLAYING;
    if (fp.st == PLAYER_STOPPED) fp.pos = 0;
    if (fp.pos > PLAYER_PREV_RESTART_MS || fp.idx == 0) {
        fp.pos = 0;
        fp.d_pos = true;
        return;
    }
    fp_load(fp.idx - 1, play);
}

/* → on Now Playing: the next track; on the last one nothing (false). */
static bool fp_next(void)
{
    fp.c_next++;
    if (fp.st == PLAYER_IDLE || fp.st == PLAYER_STOPPED || fp.idx >= fp.n - 1) return false;
    fp_load(fp.idx + 1, fp.st == PLAYER_PLAYING);
    return true;
}

static int fp_volume_step(int d)
{
    fp.c_vol++;
    int v = fp.vol + d;
    if (v < 0) v = 0;
    if (v > fp.vmax) v = fp.vmax;
    if (v != fp.vol) { fp.vol = v; fp.d_vol = true; }
    return fp.vol;
}

static player_state_t fp_state(void)    { return fp.st; }
static int64_t fp_current_id(void)      { return fp.st != PLAYER_IDLE && fp.idx >= 0 ? fp.ids[fp.idx] : 0; }
static const char *fp_current_path(void){ return fp.st != PLAYER_IDLE ? fp.path0 : ""; }
static int fp_current_index(void)       { return fp.st != PLAYER_IDLE ? fp.idx : -1; }
static int fp_queue_len(void)           { return fp.n; }
static int64_t fp_position_ms(void)     { return fp.pos; }
static int64_t fp_duration_ms(void)     { return fp.dur; }
static int fp_volume(void)              { return fp.vol; }
static int fp_volume_max(void)          { return fp.vmax; }

static const music_player_ops_t fp_ops = {
    .set_callbacks = fp_set_callbacks,
    .play_queue    = fp_play_queue,
    .toggle        = fp_toggle,
    .restart       = fp_restart,
    .prev          = fp_prev,
    .next          = fp_next,
    .volume_step   = fp_volume_step,
    .state         = fp_state,
    .current_id    = fp_current_id,
    .current_path  = fp_current_path,
    .current_index = fp_current_index,
    .queue_len     = fp_queue_len,
    .position_ms   = fp_position_ms,
    .duration_ms   = fp_duration_ms,
    .volume        = fp_volume,
    .volume_max    = fp_volume_max,
};

/* Like player_tick(): callbacks fire only from here. */
static void fp_tick_cb(lv_timer_t *t)
{
    (void)t;
    void *ud = fp.cb.ud;
    if (fp.d_track && fp.cb.track_changed)    { fp.d_track = false; fp.cb.track_changed(ud); }
    if (fp.d_state && fp.cb.state_changed)    { fp.d_state = false; fp.cb.state_changed(ud); }
    if (fp.d_vol && fp.cb.volume_changed)     { fp.d_vol = false;   fp.cb.volume_changed(ud); }
    if (fp.d_pos && fp.cb.position_changed)   { fp.d_pos = false;   fp.cb.position_changed(ud); }
}

/* "mpv" reports progress. */
static void fp_set_position(int64_t pos, int64_t dur)
{
    fp.pos = pos;
    fp.dur = dur;
    fp.d_pos = true;
}

/* The track ended by itself (eof): the next one, or stop after the last. */
static void fp_eof(void)
{
    if (fp.st != PLAYER_PLAYING && fp.st != PLAYER_PAUSED) return;
    if (fp.idx < fp.n - 1) {
        fp_load(fp.idx + 1, fp.st == PLAYER_PLAYING);
        return;
    }
    fp.st  = PLAYER_STOPPED;        /* end of queue: last track, 0:00 */
    fp.pos = 0;
    fp.d_state = fp.d_pos = true;
}

/* ---- Library fixture --------------------------------------------------------- */

static library_t *g_mlib = NULL;
static music_test_state_t M;

static void ms(void) { ui_music_test_state(&M); }

/* Albums in display order (by title; non-ASCII sorts after 'z'):
 *   Bedtime Bananas (The Wiggly Bananas, 2021, 2 songs, no art)
 *   Double (Zed Band, 2018, 2 songs, cover.png)
 *   Fruit Salad Party (The Wiggly Bananas, 2019, 3 songs, cover.jpg)
 *   Songs (abba kids, 2020, 3 songs, no art)
 *   Été en chansons (Émile & Les Enfants, 2017, 1 song, no art) */
enum { AL_BEDTIME = 0, AL_DOUBLE, AL_FSP, AL_SONGS, AL_ETE };

static void put_small_library(void)
{
    put("abba kids/Songs/01 One.mp3",   "title=One\nartist=abba kids\nalbum=Songs\ntrack=1\nyear=2020\ndur=61000\n");
    put("abba kids/Songs/02 Two.mp3",   "title=Two\nartist=abba kids\nalbum=Songs\ntrack=2\nyear=2020\ndur=62000\n");
    put("abba kids/Songs/03 Three.mp3", "title=Three\nartist=abba kids\nalbum=Songs\ntrack=3\nyear=2020\ndur=63000\n");

    put("The Wiggly Bananas/Fruit Salad Party/cover.jpg", "COVER-FSP");
    put("The Wiggly Bananas/Fruit Salad Party/01 Peel It Up.mp3",
        "title=Peel It Up\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\ntrack=1\nyear=2019\ndur=185000\n");
    put("The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3",
        "title=Monkey Business\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\ntrack=2\nyear=2019\ndur=150000\n");
    put("The Wiggly Bananas/Fruit Salad Party/03 Banana Split.mp3",
        "title=Banana Split\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\ntrack=3\nyear=2019\ndur=120000\n");
    put("The Wiggly Bananas/Bedtime Bananas/01 Sleepy.mp3",
        "title=Sleepy\nartist=The Wiggly Bananas\nalbum=Bedtime Bananas\ntrack=1\nyear=2021\ndur=200000\n");
    put("The Wiggly Bananas/Bedtime Bananas/02 Yawn.mp3",
        "title=Yawn\nartist=The Wiggly Bananas\nalbum=Bedtime Bananas\ntrack=2\nyear=2021\ndur=210000\n");

    put("Zed Band/Double/cover.png", "COVER-DBL");
    put("Zed Band/Double/01 D1.mp3", "title=D1\nartist=Zed Band\nalbum=Double\ntrack=1\nyear=2018\ndur=90000\n");
    put("Zed Band/Double/02 D2.mp3", "title=D2\nartist=Zed Band\nalbum=Double\ntrack=2\nyear=2018\ndur=95000\n");

    put("\xC3\x89mile & Les Enfants/\xC3\x89t\xC3\xA9 en chansons/01 Ca va.mp3",
        "title=\xC3\x87" "a va\nartist=\xC3\x89mile & Les Enfants\nalbum=\xC3\x89t\xC3\xA9 en chansons\n"
        "track=1\nyear=2017\ndur=100000\n");
}

/* Enough albums that the list must scroll (virtualised rows). */
static void put_big_library(int n)
{
    char rel[256], tags[256];
    for (int i = 0; i < n; i++) {
        snprintf(rel, sizeof(rel), "Band %02d/Album %02d/01 Song %02d.mp3", i, i, i);
        snprintf(tags, sizeof(tags), "title=Song %02d\nartist=Band %02d\nalbum=Album %02d\ntrack=1\n", i, i, i);
        put(rel, tags);
    }
}

static void index_pass(void)
{
    indexer_opts_t o = { .music_dir = g_music, .db_path = g_db, .art_dir = g_art,
                         .ops = &g_stub_ops };
    indexer_t *ix = indexer_open(&o);
    if (!ix) {
        fprintf(stderr, "test: indexer_open failed\n");
        exit(2);
    }
    indexer_stats_t st;
    memset(&st, 0, sizeof(st));
    if (indexer_pass(ix, &st) != 0) fprintf(stderr, "test: indexer pass failed\n");
    indexer_close(ix);
}

/* RGBA → PNG file, uncompressed (stored deflate blocks), with stdio and
 * malloc: LVGL's lodepng encoder allocates from the LVGL heap (a 1280×800
 * frame doesn't fit next to the image cache) and writes through lv_fs. */
static uint32_t png_crc_table[256];

static uint32_t png_crc(uint32_t crc, const unsigned char *p, size_t n)
{
    if (!png_crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            png_crc_table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = png_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static bool png_chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    unsigned char hdr[8];
    put_be32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = png_crc(png_crc(0, hdr + 4, 4), data, n);
    unsigned char c[4];
    put_be32(c, crc);
    return fwrite(hdr, 1, 8, f) == 8 && fwrite(data, 1, n, f) == n && fwrite(c, 1, 4, f) == 4;
}

static unsigned png_write(const char *path, const unsigned char *rgba, unsigned w, unsigned h)
{
    size_t row = (size_t)w * 4 + 1, raw_n = row * h;
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    unsigned char *z = malloc(z_n);
    if (!z) return 1;
    unsigned char *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    size_t left = raw_n, pos = 0;
    while (left) {
        size_t n = left > 65535 ? 65535 : left;
        *o++ = left == n ? 1 : 0;
        o[0] = (unsigned char)n; o[1] = (unsigned char)(n >> 8);
        o[2] = (unsigned char)~n; o[3] = (unsigned char)(~n >> 8);
        o += 4;
        for (size_t i = 0; i < n; i++, pos++) {
            size_t y = pos / row, x = pos % row;
            unsigned char v = x == 0 ? 0 : rgba[y * w * 4 + (x - 1)];
            *o++ = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        left -= n;
    }
    put_be32(o, (b << 16) | a);
    o += 4;

    unsigned err = 0;
    FILE *f = fopen(path, "wb");
    unsigned char ihdr[13];
    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (!f || fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) != 8 ||
        !png_chunk(f, "IHDR", ihdr, 13) || !png_chunk(f, "IDAT", z, (size_t)(o - z)) ||
        !png_chunk(f, "IEND", NULL, 0))
        err = 2;
    if (f) fclose(f);
    free(z);
    return err;
}

/* A real PNG (a two-colour diagonal pattern with rings) over a thumbnail
 * the stub thumbnailer wrote as text. */
static void write_png(const char *path, int size, uint32_t c1, uint32_t c2)
{
    unsigned char *px = malloc((size_t)size * size * 4);
    int cx = size / 2, cy = size / 2;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int t = (x + y) * 255 / (2 * size);
            int dx = x - cx, dy = y - cy;
            int ring = ((dx * dx + dy * dy) * 8 / (size * size / 4)) % 2;
            unsigned char *p = px + ((size_t)y * size + x) * 4;
            for (int k = 0; k < 3; k++) {
                int a = (c1 >> (16 - 8 * k)) & 0xFF, b = (c2 >> (16 - 8 * k)) & 0xFF;
                int v = (a * (255 - t) + b * t) / 255;
                if (ring) v = v * 3 / 4;
                p[k] = (unsigned char)v;
            }
            p[3] = 255;
        }
    png_write(path, px, (unsigned)size, (unsigned)size);
    free(px);
}

/* The art id of album `album_title`. */
static int64_t album_art_id(const char *album_title)
{
    static library_all_albums_t albums;
    library_load_all_albums(g_mlib, &albums);
    for (int i = 0; i < albums.count; i++)
        if (strcmp(albums.items[i].title, album_title) == 0) return albums.items[i].art_id;
    return 0;
}

static void write_real_art(int64_t art_id, uint32_t c1, uint32_t c2)
{
    char path[PATH_MAX];
    if (library_art_path(g_mlib, art_id, 96, path, sizeof(path)))  write_png(path, 96, c1, c2);
    if (library_art_path(g_mlib, art_id, 256, path, sizeof(path))) write_png(path, 256, c1, c2);
    if (library_art_path(g_mlib, art_id, 512, path, sizeof(path))) write_png(path, 512, c1, c2);
}

/* Fresh shell booted into music, a library of `big` albums (0 = the small
 * library), the fake player, music on screen. `with_db` false leaves the
 * DB missing ("No music yet"). `real_art` writes real PNGs for Fruit Salad
 * Party and Double. */
static void music_setup(bool with_db, int big, bool real_art)
{
    shell_setup("music");
    fixture_make_dirs("test_keyboard_music");
    if (with_db) {
        if (big) put_big_library(big);
        else put_small_library();
        index_pass();
    }
    fp_reset();
    g_mlib = library_open(g_db, g_art);
    if (with_db && real_art) {
        write_real_art(album_art_id("Fruit Salad Party"), 0xFFD23F, 0xF7630C);
        write_real_art(album_art_id("Double"), 0x3FA9F5, 0x7B2CBF);
    }
    g_fp_timer = lv_timer_create(fp_tick_cb, 16, NULL);
    app_music_test_clear_ctx();
    app_music_configure(g_mlib, &fp_ops);
    clock_advance(100);
    app_music_test_reset_counters();
    ms();
}

static void music_teardown(void)
{
    app_music_configure(NULL, NULL);
    if (g_fp_timer) lv_timer_delete(g_fp_timer);
    g_fp_timer = NULL;
    library_close(g_mlib);
    g_mlib = NULL;
    fixture_remove();
    shell_teardown();
}

/* Run the fake player's tick and LVGL for a moment. */
static void settle(void)
{
    clock_advance(40);
    ms();
}

static void snapshot(const char *name)
{
    char dir[PATH_MAX], path[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir, sizeof(dir), "%s%schatbox-snapshots", tmp,
             tmp[strlen(tmp) - 1] == '/' ? "" : "/");
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    unsigned char *rgba = malloc((size_t)TEST_SCR_W * TEST_SCR_H * 4);
    for (int i = 0; i < TEST_SCR_W * TEST_SCR_H; i++) {
        uint32_t c = g_frame[i];            /* XRGB8888 */
        rgba[i * 4 + 0] = (c >> 16) & 0xFF;
        rgba[i * 4 + 1] = (c >> 8) & 0xFF;
        rgba[i * 4 + 2] = c & 0xFF;
        rgba[i * 4 + 3] = 0xFF;
    }
    unsigned err = png_write(path, rgba, TEST_SCR_W, TEST_SCR_H);
    free(rgba);
    g_tests++;
    if (err) {
        fprintf(stderr, "[FAIL] snapshot %s: lodepng error %u\n", path, err);
        g_failures++;
    } else {
        fprintf(stderr, "[PASS] snapshot written: %s\n", path);
    }
}

static bool music_focus_ok(void)
{
    return lv_indev_get_group(g_test_indev) == music_grp() &&
           lv_group_get_obj_count(music_grp()) == 1 &&
           lv_group_get_focused(music_grp()) == app_music_get_sink();
}

/* The album grid is 4 cards across (ui_music.c GRID_COLS). */
#define GRID_COLS 4

/* Move the selection to index `i` with taps: on the grid row by row, then
 * across; on an album page up and down its songs. */
static void go_to(int i)
{
    ms();
    for (int guard = 0; guard < 200; guard++) {
        if (M.focus_col == MUSIC_COL_SONGS) {
            if (M.sel[1] == i) return;
            tap(M.sel[1] < i ? LV_KEY_DOWN : LV_KEY_UP);
        } else {
            int s0 = M.sel[0];
            if (s0 == i) return;
            if (s0 / GRID_COLS != i / GRID_COLS)
                tap(s0 / GRID_COLS < i / GRID_COLS ? LV_KEY_DOWN : LV_KEY_UP);
            else
                tap(s0 < i ? LV_KEY_RIGHT : LV_KEY_LEFT);
        }
        ms();
    }
    fprintf(stderr, "  go_to(%d): never got there\n", i);
}

/* From the grid: album `i`'s page (Enter opens it, plays nothing). */
static void open_album(int i)
{
    go_to(i);
    tap(LV_KEY_ENTER);
    ms();
}

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Every call the UI can make that changes what plays or how loud. */
static int fp_calls(void)
{
    return fp.c_queue + fp.c_toggle + fp.c_restart + fp.c_prev + fp.c_next + fp.c_vol;
}

/* → on a song does nothing (§8.2, D4; only Enter plays): send → to the
 * focused Songs column (a tap, or a Pi / SDL hold) and check that no player
 * call was made, the queue, current song, position and state are as they
 * were, the view stays on the columns with Songs focused on the same rows,
 * and exactly one right bounce is the only feedback. */
enum { RIGHT_TAP, RIGHT_HOLD_PI, RIGHT_HOLD_SDL };

static bool right_on_song_noop(int how)
{
    ms();
    bool on_songs = M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS;
    int calls = fp_calls(), n = fp.n, idx = fp.idx, b = M.bounces;
    int s0 = M.sel[0], s1 = M.sel[1];
    int64_t pos = fp.pos, cur = fp_current_id();
    player_state_t st = fp.st;
    if (how == RIGHT_HOLD_PI)       hold_pi(LV_KEY_RIGHT, 1500);
    else if (how == RIGHT_HOLD_SDL) hold_sdl(LV_KEY_RIGHT, 1500);
    else                            tap(LV_KEY_RIGHT);
    settle();
    clock_advance(500);
    ms();
    return on_songs && fp_calls() == calls && fp.n == n && fp.idx == idx && fp.pos == pos &&
           fp.st == st && fp_current_id() == cur &&
           M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
           ui_music_key_ctx() == KEY_CTX_MUSIC_BROWSE &&
           M.sel[0] == s0 && M.sel[1] == s1 &&
           M.bounces == b + 1 && M.last_bounce == MUSIC_BOUNCE_RIGHT &&
           !shell_switcher_is_open() && music_focus_ok();
}

/* ---- Group 21: grid and album page navigation (§8.2, ignored keys) --------- */

static void test_group21_music_columns(void)
{
    fprintf(stderr, "\n=== Group 21: Music: grid and album page navigation ===\n");
    music_setup(true, 0, false);

    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_ALBUMS &&
                M.grid_visible && !M.page_visible,
                "T21.1a: library ready -> the album grid");
    assert_true(M.count[0] == 5 && M.sel[0] == AL_BEDTIME, "T21.1b: all 5 albums, the first selected");
    assert_true(M.count[1] == 2 && M.sel[1] == 0,
                "T21.1c: Songs follow: Bedtime Bananas' 2 songs, the first selected");
    assert_true(ui_music_key_ctx() == KEY_CTX_MUSIC_BROWSE, "T21.1d: key context = Music browse");
    assert_true(M.sel_row_focused[0], "T21.1d2: the selected card wears the focus ring");
    assert_true(M.strip_visible && M.strip_idle, "T21.1e: mini strip visible, idle (nothing playing)");
    assert_true(music_focus_ok(), "T21.1f: keyboard on music's one key sink");

    tap(LV_KEY_UP);
    ms();
    assert_true(M.sel[0] == 0 && M.bounces == 1 && M.last_bounce == MUSIC_BOUNCE_UP,
                "T21.2a: Up on the top row bounces, no wrap");
    tap(LV_KEY_LEFT);
    ms();
    assert_true(M.sel[0] == 0 && M.bounces == 2 && M.last_bounce == MUSIC_BOUNCE_LEFT,
                "T21.2b: Left on the first card of a row bounces, no wrap");

    tap(LV_KEY_RIGHT);
    ms();
    assert_true(M.sel[0] == AL_DOUBLE && M.count[1] == 2 && M.sel[1] == 0,
                "T21.3a: Right selects the next album (Double), its songs loaded");
    tap(LV_KEY_RIGHT);
    ms();
    assert_true(M.sel[0] == AL_FSP && M.count[1] == 3, "T21.3b: Fruit Salad Party: 3 songs");
    tap(LV_KEY_RIGHT);
    ms();
    int b = M.bounces;
    tap(LV_KEY_RIGHT);
    ms();
    assert_true(M.sel[0] == AL_SONGS && M.bounces == b + 1 && M.last_bounce == MUSIC_BOUNCE_RIGHT,
                "T21.3c: Right on the last card of a row bounces, no wrap");
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.sel[0] == AL_ETE && M.count[1] == 1,
                "T21.3d: Down into a shorter last row: its last card (non-ASCII title last), 1 song");
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.sel[0] == AL_ETE && M.bounces == b + 2 && M.last_bounce == MUSIC_BOUNCE_DOWN,
                "T21.4: Down on the last row bounces, no wrap");
    tap(LV_KEY_UP);
    ms();
    assert_true(M.sel[0] == 0, "T21.5: Up from the second row: the card above");

    open_album(AL_FSP);
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 0 && M.view == MUSIC_VIEW_BROWSE &&
                M.page_visible && !M.grid_visible && fp.c_queue == 0 &&
                streq(M.page_title, "Fruit Salad Party") && M.sel_row_focused[1],
                "T21.6a: Enter on an album opens its page (first song focused), plays nothing");
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.sel[1] == 1, "T21.6b: Down in Songs");
    tap(LV_KEY_DOWN);
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.sel[1] == 2 && M.last_bounce == MUSIC_BOUNCE_DOWN, "T21.6c: last song: bounce");
    tap(LV_KEY_UP);
    ms();
    int before_sel[2] = { M.sel[0], M.sel[1] };
    tap(LV_KEY_LEFT);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.grid_visible && M.sel[0] == before_sel[0] &&
                M.sel[1] == before_sel[1], "T21.7a: Left: back to the grid, both selections kept");
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 1,
                "T21.7b: Enter again: the page, on the song last selected there");
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.grid_visible && !shell_switcher_is_open(),
                "T21.7c: Esc on an album page: back to the grid (music consumes it)");

    /* Ignored keys (§8.2 last row): Tab, letters, digits, Backspace. */
    int calls_before = fp.c_queue + fp.c_toggle + fp.c_vol;
    music_test_state_t before = M;
    tap(LV_KEY_NEXT);
    tap(LV_KEY_PREV);
    tap('a');
    tap('7');
    tap(LV_KEY_BACKSPACE);
    tap(LV_KEY_HOME);
    ms();
    assert_true(M.view == before.view && M.focus_col == before.focus_col &&
                M.sel[0] == before.sel[0] && M.sel[1] == before.sel[1] &&
                M.bounces == before.bounces && !M.vol_visible,
                "T21.8a: Tab, letters, digits, Backspace do nothing");
    assert_true(fp.c_queue + fp.c_toggle + fp.c_vol == calls_before,
                "T21.8b: ...and never reach the player");
    assert_true(music_focus_ok(), "T21.8c: focus never leaves music's key sink");
    tap(LV_KEY_MENU);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == before.focus_col &&
                M.sel[0] == before.sel[0] && M.bounces == before.bounces + 1 &&
                fp.c_queue + fp.c_toggle + fp.c_vol == calls_before,
                "T21.8d: Win with nothing loaded: a bounce, no Now Playing, no player call");

    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open() && switcher_get_current() == 0,
                "T21.9a: Esc on the grid opens the switcher (music doesn't consume it)");
    tap(LV_KEY_ESC);
    ms();
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music &&
                M.view == MUSIC_VIEW_BROWSE && M.focus_col == before.focus_col &&
                M.sel[0] == before.sel[0], "T21.9b: Esc closes it, music unchanged");
    assert_true(music_focus_ok(), "T21.9c: keyboard back on music's key sink");
    music_teardown();
}

/* ---- Group 22: Enter / Space / playing markers (§7.3, D4) --------------------- */

static void test_group22_music_play(void)
{
    fprintf(stderr, "\n=== Group 22: Music: play semantics (Enter, Space) ===\n");
    music_setup(true, 0, false);

    /* Space with nothing loaded: toggle is a no-op. */
    tap(' ');
    settle();
    assert_true(fp.c_toggle == 1 && fp.st == PLAYER_IDLE && M.strip_idle,
                "T22.1: Space with nothing loaded: no-op (still idle)");

    /* Enter on an album opens its page; Enter on its first song: the album
     * from track 1, straight into Now Playing. */
    open_album(AL_FSP);
    assert_true(fp.c_queue == 0 && M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 0,
                "T22.2: Enter on an album: its page, nothing plays");
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 1 && fp.last_n == 3 && fp.last_start == 0 &&
                fp.st == PLAYER_PLAYING, "T22.2a: Enter on its first song: its 3 songs from track 1");
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && ui_music_key_ctx() == KEY_CTX_NOW_PLAYING &&
                streq(M.np_title, "Peel It Up") && streq(M.np_count, "Song 1 of 3"),
                "T22.2b: ...and Now Playing opens on track 1");
    assert_true(strstr(fp.path0, "Peel It Up.mp3") != NULL && fp.ids[0] != 0,
                "T22.2c: queue holds ids and paths from the library");
    assert_true(M.eq_animating && music_focus_ok(),
                "T22.2d: the equaliser animates; focus on music's sink");

    /* ←/→ skip tracks at once: no extra key after Enter. */
    tap(LV_KEY_RIGHT);
    settle();
    assert_true(fp.c_next == 1 && streq(M.np_title, "Monkey Business") &&
                streq(M.np_count, "Song 2 of 3") && fp.st == PLAYER_PLAYING,
                "T22.3a: Right right after Enter: the next track");
    tap(LV_KEY_LEFT);
    settle();
    assert_true(fp.c_prev == 1 && streq(M.np_title, "Peel It Up"),
                "T22.3b: Left near the start: the previous track");
    tap(LV_KEY_RIGHT);
    settle();

    /* Esc, ←, ↑: Now Playing → the album page → the grid → bounce. */
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS && M.page_visible &&
                M.sel[0] == AL_FSP && M.sel[1] == 1 && M.sel_id[1] == fp_current_id() &&
                !shell_switcher_is_open(),
                "T22.4a: Esc on Now Playing: the playing album's page, on the playing song");
    assert_true(M.strip_visible && streq(M.strip_title, "Monkey Business"),
                "T22.4b: the strip shows the track");
    tap(LV_KEY_LEFT);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.sel[0] == AL_FSP && M.sel[1] == 1,
                "T22.4c: Left on the album page: the grid, selections kept");
    int b = M.bounces;
    tap(LV_KEY_UP);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.bounces == b + 1 &&
                M.last_bounce == MUSIC_BOUNCE_UP && fp.st == PLAYER_PLAYING,
                "T22.4d: Up on the grid's top row: bounce (music plays on)");

    /* The playing album and song are marked. */
    assert_true(M.marked_id[0] == M.sel_id[0] && M.marked_rows[0] == 1 &&
                M.marked_rows[1] == 1 && M.marked_id[1] == fp_current_id(),
                "T22.5a: playing marker on the album and on the song");
    tap(LV_KEY_LEFT);         /* Double: not playing */
    tap(LV_KEY_ENTER);        /* its page */
    ms();
    assert_true(M.marked_rows[0] == 1 && M.marked_id[0] != M.sel_id[0] && M.marked_rows[1] == 0,
                "T22.5b: another album: its songs unmarked, the playing album still marked");
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);        /* Fruit Salad Party */

    /* Enter on the album of the current song: its page, on the playing song
     * (§7.4); nothing restarts. */
    fp_set_position(42000, 150000);
    settle();
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 1 && fp.pos == 42000 && fp.st == PLAYER_PLAYING &&
                M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                M.sel[1] == 1 && M.sel_id[1] == fp_current_id(),
                "T22.6a: Enter on the playing album: its page, on the playing song; nothing restarts");
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.grid_visible && M.sel[0] == AL_FSP &&
                !shell_switcher_is_open() && fp.st == PLAYER_PLAYING,
                "T22.6b: Esc on the album page: the grid (no switcher), plays on");
    tap(LV_KEY_ENTER);
    tap(LV_KEY_UP);           /* Peel It Up */

    /* Enter on a song that isn't current: the album from that song. */
    tap(LV_KEY_DOWN);
    tap(LV_KEY_DOWN);         /* Banana Split */
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 2 && fp.last_n == 3 && fp.last_start == 2 &&
                M.view == MUSIC_VIEW_NOW_PLAYING &&
                streq(M.np_title, "Banana Split") && streq(M.np_count, "Song 3 of 3"),
                "T22.7a: Enter on a song that isn't current: album from that song, Now Playing");
    tap(LV_KEY_LEFT);
    settle();
    assert_true(streq(M.np_title, "Monkey Business"), "T22.7b: ...Left still reaches earlier tracks");

    /* Enter on the current song while it plays: from 0:00 (Enter only plays). */
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 1, "T22.8a: Esc: on the playing song");
    fp_set_position(42000, 150000);
    settle();
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 3 && fp.last_start == 1 && fp.st == PLAYER_PLAYING &&
                fp.pos == 0 && fp.c_toggle == 1,
                "T22.8b: Enter on the playing current song: restarts it from 0:00, never pauses");
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && streq(M.np_title, "Monkey Business") &&
                streq(M.np_elapsed, "0:00") && streq(M.np_state, "Now Playing"),
                "T22.8c: ...Now Playing opens at 0:00, playing");

    /* Space pauses; Enter on the paused current song plays it from 0:00. */
    fp_set_position(42000, 150000);
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PAUSED && !M.eq_animating && streq(M.np_state, "Paused"),
                "T22.9a: Space pauses (equaliser still)");
    tap(LV_KEY_ESC);
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PLAYING && fp.pos == 42000 && M.view == MUSIC_VIEW_BROWSE,
                "T22.9b: Space on the album page resumes (same position)");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PAUSED && M.view == MUSIC_VIEW_BROWSE,
                "T22.9c: Space on the album page pauses");
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 4 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                M.view == MUSIC_VIEW_NOW_PLAYING,
                "T22.9d: Enter on the paused current song: plays it from 0:00, Now Playing");
    tap(' ');
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PLAYING && fp.c_toggle == 6, "T22.9e: Space again: pause, resume");

    /* Stopped at the end of the queue. */
    tap(LV_KEY_RIGHT);        /* Banana Split, the last of the album */
    settle();
    fp_set_position(100000, 120000);
    fp_eof();
    settle();
    assert_true(fp.st == PLAYER_STOPPED && streq(M.np_title, "Banana Split") &&
                streq(M.np_elapsed, "0:00"), "T22.10a: end of queue: stopped, last track at 0:00");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_STOPPED && fp.pos == 0 && M.view == MUSIC_VIEW_NOW_PLAYING,
                "T22.10b: Space when stopped (Now Playing): nothing, no replay");
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.marked_rows[1] == 1 && M.marked_id[1] == M.sel_id[1] &&
                streq(M.strip_time, "0:00 / 2:00"),
                "T22.10c: the stopped last track stays current, selected and marked");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_STOPPED, "T22.10d: Space when stopped (album page): nothing");
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 5 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                M.view == MUSIC_VIEW_NOW_PLAYING, "T22.10e: Enter on it plays it again from 0:00");
    fp_eof();
    settle();
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_restart == 1 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                streq(M.np_title, "Banana Split") && M.view == MUSIC_VIEW_NOW_PLAYING,
                "T22.10f: stopped, Enter on Now Playing: the last song again from 0:00");

    /* Markers follow track changes from the player. */
    tap(LV_KEY_ESC);
    tap(LV_KEY_UP);
    tap(LV_KEY_UP);
    tap(LV_KEY_ENTER);        /* Peel It Up, queue of 3 */
    tap(LV_KEY_ESC);
    settle();
    int64_t first = M.marked_id[1];
    fp_eof();
    settle();
    assert_true(M.marked_id[1] != first && M.marked_rows[1] == 1 &&
                streq(M.strip_title, "Monkey Business"),
                "T22.11: eof -> next track: the song marker moves with it");

    /* Hidden music: no animation; back: animates again. */
    tap(LV_KEY_ESC);          /* the grid */
    tap(LV_KEY_ESC);          /* the switcher */
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    ms();
    assert_true(shell_visible_app() == &app_chat && !M.eq_animating && fp.st == PLAYER_PLAYING,
                "T22.12a: in chat: music keeps playing, no equaliser work");
    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    tap(LV_KEY_ENTER);
    ms();
    assert_true(shell_visible_app() == &app_music && M.eq_animating,
                "T22.12b: back in music: the equaliser animates");
    music_teardown();
}

/* ---- Group 23: → on Songs and Now Playing (§8.2 →, §8.3) -------------------------- */

static void test_group23_music_now_playing(void)
{
    fprintf(stderr, "\n=== Group 23: Music: Now Playing ===\n");
    music_setup(true, 0, false);

    open_album(AL_FSP);
    tap(LV_KEY_DOWN);         /* Monkey Business, not current */
    assert_true(right_on_song_noop(RIGHT_TAP) && fp.st == PLAYER_IDLE && fp.c_queue == 0,
                "T23.1a: Right on a song, nothing loaded: nothing (a bounce only, no play)");
    assert_true(right_on_song_noop(RIGHT_HOLD_PI) && fp.st == PLAYER_IDLE,
                "T23.1b: held Right on a song (Pi repeat timing), nothing loaded: nothing");
    assert_true(right_on_song_noop(RIGHT_HOLD_SDL) && fp.st == PLAYER_IDLE,
                "T23.1c: held Right on a song (SDL repeats), nothing loaded: nothing");

    /* Enter plays it (album from that song) and opens Now Playing. */
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == 1 && fp.last_n == 3 && fp.last_start == 1 && fp.st == PLAYER_PLAYING,
                "T23.1d: Enter on a song that isn't current starts it (album from that song)");
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && ui_music_key_ctx() == KEY_CTX_NOW_PLAYING,
                "T23.1e: ...and opens Now Playing");
    assert_true(streq(M.np_title, "Monkey Business") && streq(M.np_artist, "The Wiggly Bananas") &&
                streq(M.np_album, "Fruit Salad Party") && streq(M.np_count, "Song 2 of 3") &&
                streq(M.np_state, "Now Playing"), "T23.1f: title, artist, album, Song N of M, state");
    assert_true(!M.strip_visible && !M.page_visible && !M.grid_visible,
                "T23.1g: the album page and strip hidden under Now Playing");
    assert_true(music_focus_ok(), "T23.1h: focus still on music's sink");

    /* Progress from position callbacks. */
    fp_set_position(61000, 150000);
    settle();
    assert_true(streq(M.np_elapsed, "1:01") && streq(M.np_remaining, "-1:29") &&
                M.np_progress == 406, "T23.2a: position callback -> 1:01, -1:29, bar 406/1000");
    fp_set_position(61900, 150000);
    settle();
    assert_true(streq(M.np_elapsed, "1:01") && M.np_progress == 412,
                "T23.2b: same second: bar moves, times unchanged");
    fp_set_position(2000, -1);
    settle();
    assert_true(streq(M.np_remaining, "-2:28"), "T23.2c: unknown mpv duration: the library's is used");

    /* →: the next track; on the last track a bounce, and it plays on. */
    int q = fp.c_queue, b = M.bounces;
    tap(LV_KEY_RIGHT);
    settle();
    assert_true(fp.c_next == 1 && streq(M.np_title, "Banana Split") &&
                streq(M.np_count, "Song 3 of 3") && fp.st == PLAYER_PLAYING && M.bounces == b,
                "T23.3a: Right on Now Playing: the next track, still playing");
    fp_set_position(30000, 120000);
    settle();
    tap(LV_KEY_RIGHT);
    settle();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && M.bounces == b + 1 &&
                M.last_bounce == MUSIC_BOUNCE_RIGHT && fp.c_queue == q &&
                streq(M.np_title, "Banana Split") && fp.st == PLAYER_PLAYING && fp.pos == 30000 &&
                streq(M.np_state, "Now Playing"),
                "T23.3b: Right on the last track: bounce only, keeps playing (no stop)");

    /* Enter: the current song again from 0:00, playing. Never a pause. */
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_restart == 1 && fp.c_toggle == 0 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                streq(M.np_title, "Banana Split") && streq(M.np_elapsed, "0:00") &&
                streq(M.np_state, "Now Playing"),
                "T23.4a: Enter on Now Playing (playing): restarts from 0:00, keeps playing");
    fp_set_position(20000, 120000);
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PAUSED && streq(M.np_state, "Paused") && M.np_paused_badge,
                "T23.4b: Space pauses (Paused + badge)");
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_restart == 2 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                streq(M.np_state, "Now Playing") && !M.np_paused_badge && streq(M.np_elapsed, "0:00"),
                "T23.4c: Enter on Now Playing while paused: restarts from 0:00 and plays");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PAUSED, "T23.4d: Space pauses");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PLAYING && streq(M.np_state, "Now Playing") && !M.np_paused_badge,
                "T23.4e: Space resumes");

    /* ←: iPod rule (in the player); the UI follows the track. */
    fp_set_position(10000, 120000);
    settle();
    tap(LV_KEY_LEFT);
    settle();
    assert_true(fp.c_prev == 1 && streq(M.np_title, "Banana Split") && fp.pos == 0,
                "T23.5a: Left more than 3 s in: restart from 0:00");
    fp_set_position(PLAYER_PREV_RESTART_MS, 120000);
    settle();
    tap(LV_KEY_LEFT);
    settle();
    assert_true(fp.c_prev == 2 && streq(M.np_title, "Monkey Business") &&
                streq(M.np_count, "Song 2 of 3") && fp.st == PLAYER_PLAYING,
                "T23.5b: Left at exactly 3 s: previous track, still playing");
    fp_set_position(PLAYER_PREV_RESTART_MS + 1, 150000);
    settle();
    tap(LV_KEY_LEFT);
    settle();
    assert_true(fp.c_prev == 3 && streq(M.np_title, "Monkey Business") && fp.pos == 0,
                "T23.5c: Left at 3.001 s: restart");
    tap(LV_KEY_LEFT);
    settle();
    assert_true(fp.c_prev == 4 && streq(M.np_title, "Peel It Up") && streq(M.np_count, "Song 1 of 3"),
                "T23.5d: Left near the start: previous track");
    fp_set_position(1000, 185000);
    settle();
    tap(LV_KEY_LEFT);
    settle();
    assert_true(fp.c_prev == 5 && streq(M.np_title, "Peel It Up") && fp.pos == 0 &&
                fp.st == PLAYER_PLAYING, "T23.5e: Left on the first track (1 s in): restart");

    /* Skipping while paused stays paused. */
    tap(' ');
    tap(LV_KEY_RIGHT);
    settle();
    assert_true(fp.c_next == 3 && streq(M.np_title, "Monkey Business") && fp.st == PLAYER_PAUSED &&
                streq(M.np_state, "Paused"), "T23.6a: Right while paused: next track, keeps paused");
    tap(LV_KEY_LEFT);
    settle();
    assert_true(streq(M.np_title, "Peel It Up") && fp.st == PLAYER_PAUSED,
                "T23.6b: Left while paused: previous track, keeps paused");
    tap(' ');
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_RIGHT);
    settle();
    assert_true(streq(M.np_title, "Banana Split") && fp.st == PLAYER_PLAYING, "T23.6c: on the last track");

    /* The natural end of the queue: stopped, Now Playing stays open. */
    fp_set_position(119000, 120000);
    fp_eof();
    settle();
    assert_true(fp.st == PLAYER_STOPPED && M.view == MUSIC_VIEW_NOW_PLAYING &&
                streq(M.np_title, "Banana Split") && M.np_progress == 0 &&
                streq(M.np_elapsed, "0:00") && streq(M.np_state, "Paused"),
                "T23.6d: end of queue: stopped at 0:00, Now Playing stays open");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_STOPPED && fp.pos == 0, "T23.6e: Space when stopped: nothing (no replay)");
    b = M.bounces;
    tap(LV_KEY_RIGHT);
    settle();
    assert_true(fp.st == PLAYER_STOPPED && M.bounces == b + 1, "T23.6f: Right when stopped: bounce");
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_restart == 3 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                streq(M.np_title, "Banana Split"),
                "T23.6g: Enter when stopped: the last song again from 0:00");

    /* Ignored keys on Now Playing. */
    int t0 = fp.c_toggle, p0 = fp.c_prev, n0 = fp.c_next, r0 = fp.c_restart, v0 = fp.c_vol;
    tap(LV_KEY_NEXT);
    tap('x');
    tap('3');
    tap(LV_KEY_BACKSPACE);
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && fp.c_toggle == t0 && fp.c_prev == p0 &&
                fp.c_next == n0 && fp.c_restart == r0 && fp.c_vol == v0 && music_focus_ok(),
                "T23.7: Tab/letters/digits/Backspace ignored");

    /* Win toggles Now Playing ⇄ the Albums overview; the song plays on. */
    int qw = fp.c_queue;
    tap(LV_KEY_MENU);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_ALBUMS &&
                M.marked_rows[0] == 1 && M.sel_id[0] == M.marked_id[0] &&
                fp.st == PLAYER_PLAYING && !shell_switcher_is_open(),
                "T23.7b: Win on Now Playing: the album grid, playing album selected, still playing");
    tap(LV_KEY_MENU);
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && streq(M.np_title, "Banana Split") &&
                fp.st == PLAYER_PLAYING && fp.c_restart == r0 && fp.c_toggle == t0 &&
                fp.c_queue == qw && music_focus_ok(),
                "T23.7c: Win on the grid: Now Playing again, same song, nothing restarted");
    hold_pi(LV_KEY_MENU, 1500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE, "T23.7d: held Win (Pi): toggles once");
    hold_sdl(LV_KEY_MENU, 1500);
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING, "T23.7e: held Win (SDL repeats): toggles once");

    /* ↑/↓: volume, like PgUp/PgDn; the track doesn't change. */
    tap_gap(LV_KEY_UP, 100);
    ms();
    assert_true(fp.vol == 45 && M.vol_visible && streq(M.vol_value, "45%") &&
                fp.c_prev == p0 && fp.c_next == n0 && streq(M.np_title, "Banana Split") &&
                M.view == MUSIC_VIEW_NOW_PLAYING, "T23.8a: Up on Now Playing: volume +5 %, overlay");
    tap_gap(LV_KEY_DOWN, 100);
    ms();
    assert_true(fp.vol == 40 && streq(M.vol_value, "40%") && fp.c_next == n0,
                "T23.8b: Down on Now Playing: volume -5 %");
    clock_advance(1600);

    /* Esc: closes Now Playing (music consumes it): Songs, on the playing song. */
    tap(LV_KEY_ESC);
    ms();
    assert_true(!shell_switcher_is_open() && M.view == MUSIC_VIEW_BROWSE &&
                M.focus_col == MUSIC_COL_SONGS && M.sel[0] == AL_FSP && M.sel[1] == 2 &&
                M.sel_id[1] == fp_current_id() && fp.st == PLAYER_PLAYING,
                "T23.9a: Esc on Now Playing: the album page, on the playing song (no switcher, plays on)");
    tap(LV_KEY_ESC);
    ms();
    assert_true(!shell_switcher_is_open() && M.focus_col == MUSIC_COL_ALBUMS && M.grid_visible,
                "T23.9b: Esc on the album page: the grid (no switcher)");
    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T23.9c: Esc on the grid: the switcher");
    tap(LV_KEY_ESC);
    ms();
    assert_true(!shell_switcher_is_open() && M.view == MUSIC_VIEW_BROWSE &&
                M.focus_col == MUSIC_COL_ALBUMS, "T23.9d: ...closing it: the grid, unchanged");
    tap(LV_KEY_ENTER);        /* the playing album's page, on the playing song */

    /* → on a song does nothing, whatever the player is doing: only Enter
     * plays. The current song (Banana Split) is selected. */
    fp_set_position(50000, 120000);
    settle();
    assert_true(M.sel_id[1] == fp_current_id(), "T23.10a: on the playing current song");
    assert_true(right_on_song_noop(RIGHT_TAP) && fp.st == PLAYER_PLAYING && fp.pos == 50000,
                "T23.10b: Right on the playing current song: nothing (no Now Playing, no restart)");
    assert_true(right_on_song_noop(RIGHT_HOLD_PI) && right_on_song_noop(RIGHT_HOLD_SDL),
                "T23.10c: held Right on the playing current song (Pi and SDL): nothing");
    tap(LV_KEY_UP);           /* Monkey Business, not current */
    assert_true(right_on_song_noop(RIGHT_TAP) && fp.st == PLAYER_PLAYING &&
                streq(M.strip_title, "Banana Split"),
                "T23.10d: Right on a song that isn't current while one plays: nothing");
    assert_true(right_on_song_noop(RIGHT_HOLD_PI) && right_on_song_noop(RIGHT_HOLD_SDL),
                "T23.10e: held Right on it (Pi and SDL): nothing");
    tap(LV_KEY_DOWN);         /* Banana Split again */
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PAUSED && M.view == MUSIC_VIEW_BROWSE, "T23.11a: Space on the album page pauses");
    assert_true(right_on_song_noop(RIGHT_TAP) && fp.st == PLAYER_PAUSED && fp.pos == 50000,
                "T23.11b: Right on the paused current song: nothing (stays paused, same position)");
    tap(' ');
    settle();
    fp_eof();                 /* Banana Split is the album's last track: stopped */
    settle();
    assert_true(fp.st == PLAYER_STOPPED && M.sel_id[1] == fp_current_id(),
                "T23.11c: end of queue: stopped, the current song still selected");
    assert_true(right_on_song_noop(RIGHT_TAP) && fp.st == PLAYER_STOPPED && fp.pos == 0,
                "T23.11d: Right on the stopped current song: nothing (no replay)");
    int q0 = fp.c_queue;
    tap(LV_KEY_ENTER);
    settle();
    assert_true(fp.c_queue == q0 + 1 && fp.last_start == 2 && fp.st == PLAYER_PLAYING && fp.pos == 0 &&
                M.view == MUSIC_VIEW_NOW_PLAYING && streq(M.np_title, "Banana Split"),
                "T23.11e: Enter on it: plays it again from 0:00, Now Playing");

    /* Esc lands on the playing album by id, also after a reload moved it. */
    put("Aardvark/Alpha/01 A.mp3", "title=A\nartist=Aardvark\nalbum=Alpha\ntrack=1\n");
    index_pass();
    app_music_poll_library();
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && M.count[0] == 6,
                "T23.12a: a library change keeps Now Playing open");
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                M.sel[0] == AL_FSP + 1 && M.sel_id[1] == fp_current_id(),
                "T23.12b: Esc: the playing album (moved down one) and song");

    /* Held keys on Now Playing: ←/→/Enter/Space once per hold (Pi and SDL). */
    tap(LV_KEY_ENTER);        /* Banana Split from 0:00, Now Playing */
    fp_set_position(1000, 120000);
    settle();
    int pp0 = fp.c_prev;
    hold_pi(LV_KEY_LEFT, 1500);
    ms();
    assert_true(fp.c_prev == pp0 + 1 && streq(M.np_title, "Monkey Business"),
                "T23.13a: held Left (Pi repeat timing): one track back");
    clock_advance(500);
    hold_sdl(LV_KEY_LEFT, 1500);
    ms();
    assert_true(fp.c_prev == pp0 + 2 && streq(M.np_title, "Peel It Up"),
                "T23.13b: held Left (SDL repeats): one track back");
    clock_advance(500);
    int nn0 = fp.c_next;
    hold_pi(LV_KEY_RIGHT, 1500);
    clock_advance(500);
    hold_sdl(LV_KEY_RIGHT, 1500);
    ms();
    assert_true(fp.c_next == nn0 + 2 && streq(M.np_title, "Banana Split"),
                "T23.13c: held Right: one track per hold (Pi and SDL)");
    clock_advance(500);
    int r1 = fp.c_restart, t1 = fp.c_toggle;
    hold_sdl(LV_KEY_ENTER, 1500);
    clock_advance(500);
    hold_sdl(' ', 1500);
    settle();
    assert_true(fp.c_restart == r1 + 1 && fp.c_toggle == t1 + 1 && fp.st == PLAYER_PAUSED,
                "T23.13d: held Enter / Space: once each");
    tap(' ');

    /* Held Esc closes Now Playing only: its repeats don't open the switcher. */
    hold_pi(LV_KEY_ESC, 1500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                !shell_switcher_is_open(),
                "T23.14a: held Esc (Pi): Now Playing closes to the album page only");
    tap_gap(LV_KEY_ESC, 500);
    ms();
    assert_true(!shell_switcher_is_open() && M.focus_col == MUSIC_COL_ALBUMS,
                "T23.14b: the next Esc press: the grid");
    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T23.14b2: and the next: the switcher");
    tap(LV_KEY_ESC);
    tap(LV_KEY_ENTER);        /* the playing album's page */
    tap(LV_KEY_ENTER);        /* the current song from 0:00: Now Playing */
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && !shell_switcher_is_open(), "T23.14c: Now Playing again");
    hold_sdl(LV_KEY_ESC, 1500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                !shell_switcher_is_open(),
                "T23.14d: held Esc (SDL repeats): Now Playing closes to the album page only");
    sdl_tap_gap(LV_KEY_ESC, 500);
    ms();
    assert_true(!shell_switcher_is_open() && M.focus_col == MUSIC_COL_ALBUMS,
                "T23.14e: after the key-up, Esc: the grid");
    music_teardown();
}

/* ---- Group 24: selection memory (§7.4) ------------------------------------------------ */

static void test_group24_music_selection_memory(void)
{
    fprintf(stderr, "\n=== Group 24: Music: selection memory ===\n");
    music_setup(true, 0, false);

    assert_true(M.sel[0] == 0 && M.sel[1] == 0, "T24.1: boot: the first album, its first song");

    /* A track of Bedtime Bananas is current (e.g. playing on from before a
     * library reload), without any key. */
    static library_all_albums_t al;
    static library_tracks_t tr;
    static library_track_info_t info0, info1;
    library_load_all_albums(g_mlib, &al);
    library_load_tracks(g_mlib, al.items[AL_BEDTIME].id, &tr);
    library_track_get(g_mlib, tr.items[0].id, &info0);          /* Sleepy */
    library_track_get(g_mlib, tr.items[1].id, &info1);          /* Yawn */
    player_track_t pt[2] = { { tr.items[0].id, info0.path }, { tr.items[1].id, info1.path } };
    fp_play_queue(pt, 2, 1);
    settle();
    assert_true(streq(M.strip_title, "Yawn") && M.marked_rows[0] == 1 &&
                M.marked_id[0] == M.sel_id[0], "T24.2a: Yawn is current, its album marked");
    assert_true(M.sel[1] == 0, "T24.2b: a track change moves no selection");
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 1,
                "T24.2c: Enter: the album page lands on the playing song");
    tap(LV_KEY_UP);           /* Sleepy */
    tap(LV_KEY_LEFT);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.sel[1] == 0,
                "T24.3a: Left (back to the grid) keeps the song selection");
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.sel[1] == 1, "T24.3b: back into the page: the playing song overrides the last selected");
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);        /* Double */
    ms();
    assert_true(M.sel[0] == AL_DOUBLE && M.sel[1] == 0,
                "T24.4a: an album that isn't playing, first time: its first song");
    tap(LV_KEY_LEFT);         /* Bedtime again */
    ms();
    assert_true(M.sel[0] == AL_BEDTIME && M.sel[1] == 1,
                "T24.4b: moving onto the playing album: the playing song");

    /* Songs remembered per album. */
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_RIGHT);        /* Fruit Salad Party */
    tap(LV_KEY_ENTER);
    tap(LV_KEY_DOWN);
    tap(LV_KEY_DOWN);         /* Banana Split */
    tap(LV_KEY_LEFT);
    tap(LV_KEY_LEFT);         /* Double */
    tap(LV_KEY_ENTER);
    tap(LV_KEY_DOWN);         /* D2 */
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);        /* Fruit Salad Party */
    ms();
    assert_true(M.sel[0] == AL_FSP && M.sel[1] == 2, "T24.5a: Fruit Salad Party: the song last selected there");
    tap(LV_KEY_LEFT);
    ms();
    assert_true(M.sel[0] == AL_DOUBLE && M.sel[1] == 1, "T24.5b: Double: its song");
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 2,
                "T24.5c: the page of an album that isn't playing: the song last selected");

    /* The selections are kept across the switcher. */
    tap(LV_KEY_LEFT);
    tap(LV_KEY_ESC);
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.sel[0] == AL_FSP && M.sel[1] == 2,
                "T24.6: switcher and back: selections kept");

    /* Leaving Now Playing: the playing album and song. */
    tap(LV_KEY_ENTER);        /* Fruit Salad Party's page, on Banana Split */
    tap(LV_KEY_UP);
    tap(LV_KEY_UP);           /* Peel It Up */
    tap(LV_KEY_ENTER);        /* plays it: Now Playing */
    tap(LV_KEY_RIGHT);        /* Monkey Business */
    tap(LV_KEY_ESC);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[0] == AL_FSP && M.sel[1] == 1 &&
                M.sel_id[1] == fp_current_id(),
                "T24.7a: Esc from Now Playing: the playing album and song (not the last selected)");
    tap(LV_KEY_LEFT);
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);
    ms();
    assert_true(M.sel[0] == AL_FSP && M.sel[1] == 1, "T24.7b: back onto it: the playing song");

    /* A generation change keeps the selections by id. */
    tap(LV_KEY_LEFT);         /* Double */
    tap(LV_KEY_ENTER);
    tap(LV_KEY_DOWN);         /* D2 */
    tap(LV_KEY_LEFT);
    ms();
    int64_t id0 = M.sel_id[0], id1 = M.sel_id[1];
    put("Aardvark/Alpha/01 A.mp3", "title=A\nartist=Aardvark\nalbum=Alpha\ntrack=1\n");
    index_pass();
    app_music_poll_library();
    ms();
    assert_true(M.count[0] == 6 && M.sel[0] == AL_DOUBLE + 1 && M.sel_id[0] == id0 &&
                M.focus_col == MUSIC_COL_ALBUMS,
                "T24.8a: new album above: the same album stays selected (by id)");
    assert_true(M.sel[1] == 1 && M.sel_id[1] == id1, "T24.8b: ...and its song");
    music_teardown();
}

/* ---- Group 25: held keys, virtualised rows ------------------------------------------ */

static void test_group25_music_held_keys(void)
{
    fprintf(stderr, "\n=== Group 25: Music: held keys, big lists ===\n");
    music_setup(true, 60, false);

    assert_true(M.count[0] == 60, "T25.1a: 60 albums");
    assert_true(M.row_widgets[0] <= 12 && M.rows_bound[0] <= 12 && M.row_widgets[1] <= 9,
                "T25.1b: cards are virtualised (three rows of cards, not one per album)");

    hold_pi(LV_KEY_DOWN, 1500);
    ms();
    int s1 = M.sel[0];
    assert_true(s1 % GRID_COLS == 0 && s1 >= 9 * GRID_COLS && s1 <= 13 * GRID_COLS,
                "T25.2a: held Down (Pi: 400 ms, then every 100 ms): a row per repeat");
    assert_true(M.count[1] == 1 && M.sel_id[0] != 0, "T25.2b: the songs follow the held selection");
    clock_advance(500);
    hold_sdl(LV_KEY_DOWN, 1500);
    ms();
    assert_true(M.sel[0] == 56 && M.last_bounce == MUSIC_BOUNCE_DOWN,
                "T25.3: held Down (SDL repeats every 33 ms): to the last row, then bounces");
    go_to(59);
    assert_true(M.sel[0] == 59 && M.rows_bound[0] <= 12 && M.sel_row_fallback[0],
                "T25.4: the last card is bound (and draws its cover)");
    snapshot("music-grid-scrolled");
    hold_pi(LV_KEY_UP, 1000);
    ms();
    assert_true(M.sel[0] < 59 && M.sel[0] > 20 && (59 - M.sel[0]) % GRID_COLS == 0,
                "T25.5: held Up repeats too, a row per repeat");
    clock_advance(500);

    hold_pi(LV_KEY_ENTER, 1500);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.view == MUSIC_VIEW_BROWSE && fp.c_queue == 0,
                "T25.6a: held Enter (Pi) on an album: its page once, plays nothing");
    clock_advance(500);
    int b = M.bounces;
    hold_pi(LV_KEY_LEFT, 1500);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.bounces == b,
                "T25.6b: held Left (Pi): back to the grid once, no bounce");
    clock_advance(500);
    hold_sdl(LV_KEY_ENTER, 1500);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.view == MUSIC_VIEW_BROWSE && fp.c_queue == 0,
                "T25.6c: held Enter (SDL) on an album: its page once (doesn't go on into Now Playing)");
    clock_advance(500);
    hold_sdl(LV_KEY_ENTER, 1500);
    settle();
    assert_true(fp.c_queue == 1 && M.view == MUSIC_VIEW_NOW_PLAYING && fp.st == PLAYER_PLAYING &&
                fp.c_toggle == 0 && fp.c_restart == 0,
                "T25.7a: held Enter on a song (SDL): plays once, Now Playing; the repeats don't restart it");
    clock_advance(500);
    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && M.view == MUSIC_VIEW_BROWSE, "T25.7b: back on the grid");
    hold_pi(LV_KEY_ENTER, 1500);
    settle();
    assert_true(fp.c_queue == 1 && M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                fp.st == PLAYER_PLAYING && fp.c_toggle == 0 && fp.c_restart == 0,
                "T25.7c: held Enter on an album (Pi): its page; the repeats play nothing");
    clock_advance(500);
    tap(LV_KEY_LEFT);
    hold_sdl(LV_KEY_ENTER, 1500);
    settle();
    assert_true(fp.c_queue == 1 && M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                fp.st == PLAYER_PLAYING && fp.c_toggle == 0 && fp.c_restart == 0,
                "T25.7d: held Enter on an album (SDL): its page; the repeats play nothing");
    clock_advance(500);
    tap(LV_KEY_ENTER);        /* plays the song: Now Playing */
    settle();
    assert_true(fp.c_queue == 2 && M.view == MUSIC_VIEW_NOW_PLAYING, "T25.7e: Enter: Now Playing");
    clock_advance(500);
    hold_sdl(' ', 1500);
    settle();
    assert_true(fp.c_toggle == 1 && fp.st == PLAYER_PAUSED, "T25.8: held Space: toggles once");
    clock_advance(500);
    hold_sdl(LV_KEY_ESC, 1500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                !shell_switcher_is_open(),
                "T25.9: held Esc (SDL) on Now Playing: back to the album page once, no switcher");
    clock_advance(500);

    /* PgUp/PgDn repeat (SDL) — the volume steps each repeat, to the cap. */
    hold_sdl(LV_KEY_PG_UP, 1000);
    settle();
    assert_true(fp.c_vol > 6 && fp.vol == 70 && M.vol_visible &&
                streq(M.vol_msg, "that's the loudest!"),
                "T25.10: held PgUp (SDL): repeats up to the cap");
    music_teardown();
}

/* ---- Group 26: volume overlay ------------------------------------------------------------- */

static void test_group26_music_volume(void)
{
    fprintf(stderr, "\n=== Group 26: Music: volume overlay ===\n");
    music_setup(true, 0, false);

    tap_gap(LV_KEY_PG_UP, 100);
    ms();
    assert_true(M.vol_visible && fp.vol == 45 && streq(M.vol_value, "45%") && streq(M.vol_msg, ""),
                "T26.1a: PgUp: +5 %, overlay shows 45%");
    assert_true(M.vol_segs_on == 9 && M.vol_segs_locked == 6,
                "T26.1b: 9 segments on, the 6 above the 70 % cap hatched");
    for (int i = 0; i < 5; i++) tap_gap(LV_KEY_PG_UP, 100);
    ms();
    assert_true(fp.vol == 70 && streq(M.vol_value, "70%") && streq(M.vol_msg, "that's the loudest!"),
                "T26.2a: at the cap: that's the loudest!");
    tap_gap(LV_KEY_PG_UP, 100);
    ms();
    assert_true(fp.vol == 70 && streq(M.vol_msg, "that's the loudest!") && M.vol_segs_on == 14,
                "T26.2b: PgUp at the cap: stays");
    clock_advance(1300);
    ms();
    assert_true(M.vol_visible, "T26.3a: still shown 1.4 s after the last key");
    clock_advance(200);
    ms();
    assert_true(!M.vol_visible, "T26.3b: hidden after 1.5 s");
    for (int i = 0; i < 14; i++) tap_gap(LV_KEY_PG_DOWN, 100);
    ms();
    assert_true(fp.vol == 0 && streq(M.vol_value, "0%") && streq(M.vol_msg, "silent") &&
                M.vol_segs_on == 0, "T26.4a: down to 0: silent");
    tap_gap(LV_KEY_PG_DOWN, 100);
    ms();
    assert_true(fp.vol == 0 && streq(M.vol_msg, "silent"), "T26.4b: PgDn at 0: stays silent");
    tap_gap(LV_KEY_PG_UP, 600);

    /* On Now Playing too. */
    open_album(AL_FSP);
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING, "T26.5a: Now Playing open");
    tap_gap(LV_KEY_PG_UP, 100);
    ms();
    assert_true(M.vol_visible && fp.vol == 10 && M.view == MUSIC_VIEW_NOW_PLAYING,
                "T26.5b: PgUp on Now Playing: volume + overlay");
    tap(LV_KEY_PG_DOWN);
    ms();
    assert_true(fp.vol == 5, "T26.5c: PgDn on Now Playing");

    /* ↑/↓ on Now Playing: the same volume steps, overlay and edges. */
    tap_gap(LV_KEY_DOWN, 100);
    ms();
    assert_true(fp.vol == 0 && streq(M.vol_value, "0%") && streq(M.vol_msg, "silent") &&
                M.vol_segs_on == 0, "T26.6a: Down on Now Playing to 0: silent");
    tap_gap(LV_KEY_DOWN, 100);
    ms();
    assert_true(fp.vol == 0 && streq(M.vol_msg, "silent") && M.view == MUSIC_VIEW_NOW_PLAYING &&
                streq(M.np_title, "Peel It Up") && fp.c_prev == 0 && fp.c_next == 0,
                "T26.6b: Down at 0: stays silent, the track doesn't change");
    tap_gap(LV_KEY_UP, 100);
    ms();
    assert_true(fp.vol == 5 && streq(M.vol_value, "5%") && streq(M.vol_msg, "") &&
                M.vol_segs_on == 1 && M.vol_segs_locked == 6, "T26.6c: Up: 5 %, overlay");
    for (int i = 0; i < 20; i++) tap_gap(LV_KEY_UP, 100);
    ms();
    assert_true(fp.vol == 70 && streq(M.vol_value, "70%") && streq(M.vol_msg, "that's the loudest!") &&
                M.vol_segs_on == 14, "T26.6d: Up to the cap: that's the loudest!");
    clock_advance(1600);
    ms();
    assert_true(!M.vol_visible, "T26.6e: overlay hides 1.5 s after the last Up");

    /* Held ↓ / ↑ on the Pi: LVGL repeats (400 ms, then every 100 ms), each
     * a volume step — they are not one-shot on Now Playing. */
    int v0 = fp.c_vol;
    hold_pi(LV_KEY_DOWN, 1000);
    ms();
    int steps = fp.c_vol - v0;
    assert_true(steps >= 6 && steps <= 8 && fp.vol == 70 - 5 * steps && M.vol_visible,
                "T26.7a: held Down (Pi) 1 s: the press + ~6 repeats, 5 % each");
    fprintf(stderr, "  held Down 1 s: %d steps, volume %d\n", steps, fp.vol);
    hold_pi(LV_KEY_DOWN, 2000);
    ms();
    assert_true(fp.vol == 0 && streq(M.vol_msg, "silent"), "T26.7b: held Down ramps to 0: silent");
    clock_advance(500);
    v0 = fp.c_vol;
    hold_pi(LV_KEY_UP, 3000);
    ms();
    assert_true(fp.c_vol - v0 >= 20 && fp.vol == 70 && streq(M.vol_msg, "that's the loudest!"),
                "T26.7c: held Up (Pi) ramps to the cap: that's the loudest!");
    clock_advance(500);
    hold_sdl(LV_KEY_DOWN, 600);
    ms();
    assert_true(fp.vol < 70 && fp.vol >= 70 - 5 * 12, "T26.7d: held Down (SDL repeats): steps down");
    assert_true(fp.c_prev == 0 && fp.c_next == 0 && fp.c_restart == 0 &&
                M.view == MUSIC_VIEW_NOW_PLAYING && streq(M.np_title, "Peel It Up"),
                "T26.7e: held Up/Down never skip tracks");
    music_teardown();
}

/* ---- Group 27: "No music yet", library appears / changes (§7.5, §7.7, §8.4) ------------- */

static void test_group27_music_library_changes(void)
{
    fprintf(stderr, "\n=== Group 27: Music: No music yet, library reloads ===\n");
    music_setup(false, 0, false);

    assert_true(M.view == MUSIC_VIEW_NO_MUSIC && ui_music_key_ctx() == KEY_CTX_NO_MUSIC,
                "T27.1a: no DB: No music yet (context No music yet)");
    assert_true(!M.strip_visible, "T27.1b: full screen, no strip");
    int q0 = fp.c_queue;
    tap(LV_KEY_UP);
    tap(LV_KEY_DOWN);
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    tap(LV_KEY_MENU);
    tap(LV_KEY_NEXT);
    tap('q');
    tap('5');
    tap(LV_KEY_BACKSPACE);
    ms();
    assert_true(M.view == MUSIC_VIEW_NO_MUSIC && fp.c_queue == q0 &&
                fp.c_toggle == 0 && fp.c_vol == 0 && M.bounces == 0 && music_focus_ok(),
                "T27.2: arrows, Enter, Win, Tab, letters, digits, Backspace ignored");
    tap(' ');
    ms();
    assert_true(fp.c_toggle == 1 && fp.st == PLAYER_IDLE, "T27.3a: Space: pause/resume if loaded (none: no-op)");
    hold_sdl(' ', 1200);
    assert_true(fp.c_toggle == 2, "T27.3b: Space is one-shot here");
    clock_advance(500);       /* the SDL key-up resets only on an SDL press */
    /* Something loaded (e.g. playing on from before the library went away):
     * Space pauses and resumes; stopped at the end: nothing. */
    player_track_t lone[1] = { { 7, "/srv/music/x.mp3" } };
    fp_play_queue(lone, 1, 0);
    settle();
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PAUSED && M.view == MUSIC_VIEW_NO_MUSIC,
                "T27.3c: Space on No music yet with a track loaded: pause");
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_PLAYING, "T27.3d: Space again: resume");
    fp_eof();
    settle();
    tap(' ');
    settle();
    assert_true(fp.st == PLAYER_STOPPED && fp.c_toggle == 5,
                "T27.3e: Space when stopped: nothing (no replay)");
    fp.st = PLAYER_IDLE;      /* back to nothing loaded for the rest */
    fp.idx = -1;
    fp.n = 0;
    fp.c_queue = 0;
    fp.d_track = fp.d_state = true;
    settle();
    tap_gap(LV_KEY_PG_UP, 100);
    ms();
    assert_true(M.vol_visible && fp.vol == 45, "T27.4: PgUp: volume + overlay on No music yet");
    clock_advance(1600);
    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T27.5a: Esc opens the switcher");
    tap(LV_KEY_ESC);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music, "T27.5b: and closes it");

    /* The library appears. */
    app_music_poll_library();
    ms();
    assert_true(M.view == MUSIC_VIEW_NO_MUSIC, "T27.6a: poll with no DB: still No music yet");
    put_small_library();
    index_pass();
    app_music_poll_library();
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_ALBUMS && M.sel[0] == 0 &&
                M.count[0] == 5, "T27.6b: library appears: the grid, the first album");
    assert_true(ui_music_key_ctx() == KEY_CTX_MUSIC_BROWSE, "T27.6c: context Music browse");

    /* Generation change keeps selections by id. */
    open_album(AL_FSP);
    tap(LV_KEY_DOWN);         /* Fruit Salad Party / Monkey Business */
    ms();
    int64_t id0 = M.sel_id[0], id1 = M.sel_id[1];
    assert_true(M.sel[0] == AL_FSP && M.sel[1] == 1, "T27.7a: Fruit Salad Party / Monkey Business");
    put("Aardvark/Alpha/01 A.mp3", "title=A\nartist=Aardvark\nalbum=Alpha\ntrack=1\n");
    put("The Wiggly Bananas/Fruit Salad Party/00 Intro.mp3",
        "title=Intro\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\ntrack=0\nyear=2019\n");
    index_pass();
    app_music_poll_library();
    ms();
    assert_true(M.count[0] == 6 && M.sel[0] == AL_FSP + 1 && M.sel_id[0] == id0,
                "T27.7b: new album above: the same album stays selected (by id, index moves)");
    assert_true(M.sel_id[1] == id1 && M.count[1] == 4,
                "T27.7c: the song too (a new song in the album)");
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.view == MUSIC_VIEW_BROWSE,
                "T27.7d: focus kept");

    /* The selected item vanishes: same index, clamped. */
    tap(LV_KEY_LEFT);
    tap(LV_KEY_LEFT);         /* Double, index 2 */
    ms();
    int64_t dbl = M.sel_id[0];
    assert_true(M.sel[0] == AL_DOUBLE + 1, "T27.8a: Double at index 2");
    rm("Zed Band/Double/01 D1.mp3");
    rm("Zed Band/Double/02 D2.mp3");
    index_pass();
    app_music_poll_library();
    ms();
    assert_true(M.count[0] == 5 && M.sel[0] == 2 && M.sel_id[0] != dbl && M.sel_id[0] == id0,
                "T27.8b: Double gone: the selection stays at index 2 (now Fruit Salad Party)");
    assert_true(M.rows_visible[0] == 5,
                "T27.8b2: the list shrank: no stale card left showing the old last album");
    go_to(4);                 /* Été en chansons, the last */
    rm("\xC3\x89mile & Les Enfants/\xC3\x89t\xC3\xA9 en chansons/01 Ca va.mp3");
    index_pass();
    app_music_poll_library();
    ms();
    assert_true(M.count[0] == 4 && M.sel[0] == 3, "T27.8c: the last album gone: clamped to the new last");
    assert_true(M.rows_visible[0] == 4, "T27.8c2: ...and only 4 cards are showing");
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.sel[0] == 3 && M.last_bounce == MUSIC_BOUNCE_DOWN, "T27.8d: ...and it is the end");

    /* The library goes away: No music yet again. */
    char p[PATH_MAX];
    unlink(g_db);
    snprintf(p, sizeof(p), "%s-wal", g_db);
    unlink(p);
    snprintf(p, sizeof(p), "%s-shm", g_db);
    unlink(p);
    app_music_poll_library();
    ms();
    assert_true(M.view == MUSIC_VIEW_NO_MUSIC, "T27.9: the DB disappears: No music yet");
    music_teardown();
}

/* ---- Group 28: fallback covers (§7.6) ------------------------------------------------------ */

static void test_group28_music_fallback_covers(void)
{
    fprintf(stderr, "\n=== Group 28: Music: artwork and fallback covers ===\n");
    music_setup(true, 0, true);

    assert_true(M.sel_row_fallback[0] && streq(M.sel_row_letter[0], "B"),
                "T28.1a: album with no art (Bedtime Bananas): fallback cover, letter B");
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.sel_row_fallback[1], "T28.1b: its songs: fallback too");
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);
    ms();
    assert_true(M.sel_row_image[0], "T28.2a: real thumbnails (Double): the card shows the image");
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.sel_row_image[1], "T28.2b: ...and its song rows");
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);
    ms();
    assert_true(M.sel_row_image[0] && M.sel_row_image[1], "T28.3: Fruit Salad Party: images");
    tap(LV_KEY_LEFT);
    tap(LV_KEY_RIGHT);
    ms();
    assert_true(M.sel_row_fallback[0] && streq(M.sel_row_letter[0], "S"),
                "T28.4a: Songs (abba kids, no art): fallback, letter S");
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.sel_row_fallback[0] && streq(M.sel_row_letter[0], "\xC3\x89"),
                "T28.4b: non-ASCII first letter (\xC3\x89t\xC3\xA9 en chansons): \xC3\x89");

    /* A thumbnail file that is missing, or not a PNG, draws the fallback. */
    go_to(AL_DOUBLE);
    assert_true(M.sel_row_image[0], "T28.5a: Double: image");
    char path[PATH_MAX];
    int64_t dbl = album_art_id("Double");
    library_art_path(g_mlib, dbl, 256, path, sizeof(path));   /* the card's */
    unlink(path);
    /* What a library change does (it drops the decoded-image cache). */
    ui_music_reload();
    ms();
    assert_true(M.sel_row_fallback[0] && streq(M.sel_row_letter[0], "D"),
                "T28.5b: thumbnail file missing: fallback cover (D)");
    spit(path, "PNG256:not really", 17);
    ui_music_reload();
    ms();
    assert_true(M.sel_row_fallback[0], "T28.5c: thumbnail that doesn't decode: fallback cover");

    /* Strip and Now Playing: fallback for no art, image for real art. */
    open_album(AL_SONGS);
    tap(LV_KEY_ENTER);
    settle();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && M.np_fallback && !M.np_image,
                "T28.6a: Now Playing, album without art: fallback cover");
    tap(LV_KEY_ESC);
    settle();
    assert_true(M.strip_fallback, "T28.6b: strip: fallback cover");
    tap(LV_KEY_LEFT);
    open_album(AL_FSP);
    tap(LV_KEY_ENTER);
    settle();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && M.np_image && !M.np_fallback,
                "T28.7a: Now Playing with real art: the 512 px image");
    tap(LV_KEY_ESC);
    settle();
    assert_true(!M.strip_fallback, "T28.7b: strip: the 96 px image");
    music_teardown();
}

/* ---- Group 29: snapshots (visual check, written to $TMPDIR/chatbox-snapshots) ----------- */

static void test_group29_music_snapshots(void)
{
    fprintf(stderr, "\n=== Group 29: Music: screen snapshots ===\n");
    music_setup(true, 0, true);

    open_album(AL_FSP);
    tap(LV_KEY_DOWN);
    tap(LV_KEY_ENTER);        /* Monkey Business: Now Playing */
    fp_set_position(83000, 150000);
    settle();
    snapshot("music-now-playing");
    tap(' ');
    settle();
    snapshot("music-now-playing-paused");
    tap(' ');

    tap(LV_KEY_ESC);
    tap(LV_KEY_UP);           /* selection on another song than the playing one */
    settle();
    snapshot("music-album-page-playing");
    tap(LV_KEY_LEFT);
    settle();
    snapshot("music-grid");

    for (int i = 0; i < 6; i++) tap_gap(LV_KEY_PG_UP, 80);
    settle();
    snapshot("music-volume-cap");
    clock_advance(1600);

    go_to(AL_BEDTIME);        /* fallback covers */
    settle();
    snapshot("music-grid-fallback");

    /* Long titles: two lines and the artist after the song count; a
     * compilation doesn't name "Various Artists". */
    put("The Wiggly Bananas/Wiggly Wonderful Christmas Holiday Songbook/01 Jingle.mp3",
        "title=Jingle All the Way Home Through the Snowy Forest\nartist=The Wiggly Bananas\n"
        "album=Wiggly Wonderful Christmas Holiday Songbook\ntrack=1\nyear=2022\ndur=100000\n");
    put("Compilations/Bedtime Lullabies/01 Hush.mp3",
        "title=Hush\nartist=Sleepy Singers\nalbum=Bedtime Lullabies\ntrack=1\nyear=2016\n");
    index_pass();
    app_music_poll_library();
    go_to(5);                 /* the Christmas songbook */
    settle();
    snapshot("music-grid-long-titles");
    tap(LV_KEY_ENTER);
    settle();
    snapshot("music-album-long-titles");
    tap(LV_KEY_ENTER);        /* a two-line title on Now Playing */
    settle();
    snapshot("music-now-playing-long-title");
    tap(LV_KEY_ESC);
    tap(LV_KEY_LEFT);
    go_to(1);                 /* Bedtime Lullabies: a compilation */
    tap(LV_KEY_ENTER);
    settle();
    snapshot("music-album-compilation");
    tap(LV_KEY_LEFT);

    tap(LV_KEY_ESC);
    clock_advance(400);
    assert_true(shell_switcher_is_open(), "T29.1: switcher open over music");
    snapshot("music-switcher");
    tap(LV_KEY_ESC);
    music_teardown();

    music_setup(false, 0, false);
    snapshot("music-no-music-yet");
    music_teardown();
}

/* ---- Group 30: live tiles (T9, §6) ---------------------------------------------------------- */

static int timer_count(void)
{
    int n = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) n++;
    return n;
}

/* `o`'s top-left relative to switcher tile `idx`'s top-left. */
static void tile_rel(lv_obj_t *o, int idx, int32_t *x, int32_t *y)
{
    lv_obj_update_layout(switcher_get_root());
    lv_area_t a, t;
    lv_obj_get_coords(o, &a);
    lv_obj_get_coords(switcher_get_tile(idx), &t);
    *x = a.x1 - t.x1;
    *y = a.y1 - t.y1;
}

static int32_t abs_y(lv_obj_t *o)
{
    lv_obj_update_layout(switcher_get_root());
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a.y1;
}

/* Caption and face of both tiles at the same place in their tile, and the
 * captions on one baseline, whatever is focused / current. */
static bool tile_layout_same(const char *what)
{
    int32_t cx[2], cy[2], fx[2], fy[2];
    for (int i = 0; i < 2; i++) {
        tile_rel(switcher_get_caption(i), i, &cx[i], &cy[i]);
        tile_rel(lv_obj_get_child(switcher_get_tile(i), 0), i, &fx[i], &fy[i]);
    }
    bool ok = cx[0] == cx[1] && cy[0] == cy[1] && fx[0] == fx[1] && fy[0] == fy[1] &&
              abs_y(switcher_get_caption(0)) == abs_y(switcher_get_caption(1));
    if (!ok)
        fprintf(stderr, "  %s: caption (%d,%d) vs (%d,%d), face (%d,%d) vs (%d,%d)\n", what,
                (int)cx[0], (int)cy[0], (int)cx[1], (int)cy[1],
                (int)fx[0], (int)fy[0], (int)fx[1], (int)fy[1]);
    return ok;
}

static char g_avatar_dir[PATH_MAX];

/* Real 128 px avatar PNGs for the fixture's contacts (mom, dad), so the
 * sidebar and the Chat tile have images. Before shell_setup: the sidebar
 * resolves avatars when it is built. */
static void avatars_setup(void)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_avatar_dir, sizeof(g_avatar_dir), "%s%savatars.XXXXXX", tmp,
             tmp[strlen(tmp) - 1] == '/' ? "" : "/");
    if (!mkdtemp(g_avatar_dir)) { perror("mkdtemp"); exit(2); }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/mom.png", g_avatar_dir);
    write_png(path, 128, 0xFF8FB1, 0xC2185B);
    snprintf(path, sizeof(path), "%s/dad.png", g_avatar_dir);
    write_png(path, 128, 0x8BC34A, 0x1B5E20);
    avatar_init(g_avatar_dir);
}

static void avatars_teardown(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/mom.png", g_avatar_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/dad.png", g_avatar_dir);
    unlink(path);
    rmdir(g_avatar_dir);
}

static app_chat_tile_state_t CT;

/* The tile shows contact `i`'s avatar file (LVGL keeps its own copy of a
 * file source, so compare the paths). */
static bool tile_avatar_is(int i)
{
    const char *want = ui_sidebar_avatar_src(i);
    return CT.avatar_src && want && strcmp(CT.avatar_src, want) == 0;
}
static void cts(void) { app_chat_test_tile(&CT); }

static void open_switcher(void)
{
    tap(LV_KEY_ESC);
    clock_advance(400);         /* fly-in done */
    ms();
    cts();
}

/* ---- Group 31: fast separate presses (release reset, §8.1) ----------------------------- */

static void test_group31_fast_taps(void)
{
    fprintf(stderr, "\n=== Group 31: Fast separate presses (release reset) ===\n");
    music_setup(true, 0, false);

    /* Pi-style keyboard: the indev's release edges are real. */
    go_to(AL_FSP);
    tap_gap(LV_KEY_ENTER, 100);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS,
                "T31.1a: Enter: the grid -> the album page");
    int b = M.bounces;
    tap_gap(LV_KEY_RIGHT, 100);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                M.bounces == b + 1 && M.last_bounce == MUSIC_BOUNCE_RIGHT && fp.c_queue == 0,
                "T31.1b: a second -> 100 ms later acts: on a song, a bounce (plays nothing)");
    tap_gap(LV_KEY_ENTER, 150);
    settle();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && fp.c_queue == 1 && fp.st == PLAYER_PLAYING,
                "T31.1c: Enter 100 ms later starts the song and opens Now Playing");
    b = M.bounces;
    tap_gap(LV_KEY_ESC, 100);
    tap_gap(LV_KEY_LEFT, 100);
    tap_gap(LV_KEY_UP, 500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_ALBUMS &&
                M.bounces == b + 1 && M.last_bounce == MUSIC_BOUNCE_UP && !shell_switcher_is_open(),
                "T31.2: Esc <- ^ 100 ms apart: Now Playing closes, page -> grid, then a bounce");

    /* Enter then → quickly: Now Playing is up at once, → skips. */
    tap(LV_KEY_ENTER);
    tap(LV_KEY_DOWN);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 1,
                "T31.3a: on song 2 (not current)");
    int queues = fp.c_queue, nexts = fp.c_next;
    tap_gap(LV_KEY_ENTER, 100);
    tap_gap(LV_KEY_RIGHT, 100);
    settle();
    assert_true(fp.c_queue == queues + 1 && fp.c_next == nexts + 1 &&
                M.view == MUSIC_VIEW_NOW_PLAYING && streq(M.np_title, "Banana Split"),
                "T31.3b: Enter, -> 100 ms later: plays song 2, then the next track");
    tap_gap(LV_KEY_LEFT, 100);
    tap_gap(LV_KEY_LEFT, 500);
    settle();
    assert_true(fp.c_prev == 2 && streq(M.np_title, "Peel It Up"),
                "T31.3c: <- <- 100 ms apart: two tracks back");

    /* Enter twice within 200 ms on a song: the second is Now Playing's
     * Enter: the song again from 0:00, still playing (never a pause). */
    tap(LV_KEY_ESC);
    tap(LV_KEY_DOWN);         /* Monkey Business */
    fp_set_position(7000, 185000);
    settle();
    queues = fp.c_queue;
    int toggles = fp.c_toggle;
    tap_gap(LV_KEY_ENTER, 50);
    fp_set_position(7000, 150000);
    clock_advance(150);
    ms();
    assert_true(fp.c_queue == queues + 1 && streq(M.np_title, "Monkey Business") &&
                M.view == MUSIC_VIEW_NOW_PLAYING, "T31.4a: first Enter plays it, Now Playing");
    tap_gap(LV_KEY_ENTER, 300);
    settle();
    assert_true(fp.c_queue == queues + 1 && fp.c_restart == 1 && fp.c_toggle == toggles &&
                fp.st == PLAYER_PLAYING && fp.pos == 0 && M.view == MUSIC_VIEW_NOW_PLAYING &&
                streq(M.np_elapsed, "0:00"),
                "T31.4b: second Enter 200 ms later: restarts from 0:00, still playing");
    tap_gap(' ', 100);
    tap_gap(' ', 500);
    assert_true(fp.c_toggle == toggles + 2 && fp.st == PLAYER_PLAYING,
                "T31.4c: Space Space 100 ms apart: pause, resume");
    tap_gap(LV_KEY_UP, 100);
    tap_gap(LV_KEY_UP, 500);
    ms();
    assert_true(fp.vol == 50, "T31.4d: Up Up 100 ms apart on Now Playing: two volume steps");

    /* Esc Esc Esc Esc quickly: Now Playing closes, the album page closes,
     * the switcher opens, closes. */
    tap_gap(LV_KEY_ESC, 100);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && !shell_switcher_is_open(),
                "T31.5a: Esc closes Now Playing");
    tap_gap(LV_KEY_ESC, 100);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS && !shell_switcher_is_open(),
                "T31.5a2: Esc 100 ms later: the grid");
    tap_gap(LV_KEY_ESC, 100);
    assert_true(shell_switcher_is_open(), "T31.5b: Esc 100 ms later opens the switcher");
    tap_gap(LV_KEY_ESC, 500);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music,
                "T31.5c: Esc 100 ms later closes it");

    /* Switcher -> then <- quickly; -> -> at the edge shakes. */
    tap_gap(LV_KEY_ESC, 100);
    int shakes = switcher_get_shake_count();
    tap_gap(LV_KEY_RIGHT, 100);
    assert_true(switcher_get_focus() == 1, "T31.6a: switcher ->: Chat");
    tap_gap(LV_KEY_LEFT, 100);
    assert_true(switcher_get_focus() == 0, "T31.6b: <- 100 ms later: Music");
    tap_gap(LV_KEY_RIGHT, 100);
    tap_gap(LV_KEY_RIGHT, 100);
    assert_true(switcher_get_focus() == 1 && switcher_get_shake_count() == shakes + 1,
                "T31.6c: -> -> 100 ms apart: Chat, then an edge shake");
    tap_gap(LV_KEY_LEFT, 100);
    tap_gap(LV_KEY_ENTER, 500);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music,
                "T31.6d: <- Enter: back to music");

    /* Pi hold: still exactly once; after the release the next press acts. */
    tap(LV_KEY_ENTER);        /* the playing album's page */
    tap(LV_KEY_ENTER);        /* the playing song from 0:00: Now Playing */
    ms();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING, "T31.7a: on Now Playing");
    hold_pi(LV_KEY_ESC, 1500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS &&
                !shell_switcher_is_open(), "T31.7b: held Esc (Pi): Now Playing -> the album page only");
    tap_gap(LV_KEY_LEFT, 500);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS,
                "T31.7c: <- 20 ms after the hold's release acts");

    /* SDL keyboard: the same with the driver's fake releases and the real
     * key-ups from the event watch. */
    int queues1 = fp.c_queue;
    b = M.bounces;
    sdl_tap_gap(LV_KEY_ENTER, 100);
    sdl_tap_gap(LV_KEY_DOWN, 100);
    sdl_tap_gap(LV_KEY_RIGHT, 100);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS && M.sel[1] == 2 &&
                M.bounces == b + 1 && M.last_bounce == MUSIC_BOUNCE_RIGHT && fp.c_queue == queues1,
                "T31.8a: SDL Enter v -> 100 ms apart: the page, next song, then a bounce (plays nothing)");
    sdl_tap_gap(LV_KEY_ENTER, 500);
    settle();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && fp.c_queue == queues1 + 1 && fp.last_start == 2 &&
                streq(M.np_title, "Banana Split"),
                "T31.8b: SDL Enter 100 ms later: plays it (not current), Now Playing");
    sdl_tap_gap(LV_KEY_ESC, 100);
    sdl_tap_gap(LV_KEY_LEFT, 500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_ALBUMS &&
                !shell_switcher_is_open(), "T31.8c: SDL Esc <- 100 ms apart: back two");
    hold_sdl(LV_KEY_ENTER, 1500);
    ms();
    assert_true(M.focus_col == MUSIC_COL_SONGS && M.view == MUSIC_VIEW_BROWSE && fp.c_queue == queues1 + 1,
                "T31.8d: SDL hold of Enter (OS repeats, no key-up): the album page once, plays nothing");
    sdl_tap_gap(LV_KEY_LEFT, 500);
    ms();
    assert_true(M.focus_col == MUSIC_COL_ALBUMS,
                "T31.8e: after the real key-up, <- 20 ms later acts");

    /* The watch runs when SDL queues an event, possibly before LVGL handles
     * the press: a key-up queued before its own press is delivered still
     * resets only after that press. */
    b = M.bounces;
    sdl_mode(true);
    shell_key_pushed(LV_KEY_RIGHT);
    shell_key_released(LV_KEY_RIGHT);
    key_down(LV_KEY_RIGHT);
    key_up();
    clock_advance(50);
    shell_key_pushed(LV_KEY_RIGHT);
    shell_key_released(LV_KEY_RIGHT);
    key_down(LV_KEY_RIGHT);
    key_up();
    clock_advance(500);
    ms();
    assert_true(M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_ALBUMS &&
                M.sel[0] == AL_SONGS && M.bounces == b + 1 && M.last_bounce == MUSIC_BOUNCE_RIGHT,
                "T31.9a: key-down + key-up queued together, twice 50 ms apart: both act (a card, a bounce)");
    /* Back on the playing album's page; then Enter (the current song from
     * 0:00) for Now Playing. */
    sdl_mode(false);
    open_album(AL_FSP);
    sdl_mode(true);
    shell_key_pushed(LV_KEY_ENTER);
    key_down(LV_KEY_ENTER);
    key_up();
    sdl_keyup(LV_KEY_ENTER);
    clock_advance(500);
    settle();
    assert_true(M.view == MUSIC_VIEW_NOW_PLAYING && fp.pos == 0 && streq(M.np_title, "Banana Split"),
                "T31.9a2: Enter: Now Playing");
    /* Two whole taps queued before LVGL handles either (← on Now Playing). */
    int prevs = fp.c_prev;
    shell_key_pushed(LV_KEY_LEFT);
    shell_key_released(LV_KEY_LEFT);
    shell_key_pushed(LV_KEY_LEFT);
    shell_key_released(LV_KEY_LEFT);
    key_down(LV_KEY_LEFT);
    key_up();
    key_down(LV_KEY_LEFT);
    key_up();
    clock_advance(500);
    ms();
    assert_true(fp.c_prev == prevs + 2 && M.view == MUSIC_VIEW_NOW_PLAYING,
                "T31.9b: two taps queued before LVGL handles them: both act");
    /* A hold whose last OS repeat and the key-up are queued together:
     * that repeat is still dropped. */
    sdl_down_gap(LV_KEY_LEFT, 300);
    sdl_down_gap(LV_KEY_LEFT, 33);
    shell_key_pushed(LV_KEY_LEFT);
    shell_key_released(LV_KEY_LEFT);
    key_down(LV_KEY_LEFT);
    key_up();
    clock_advance(50);
    ms();
    assert_true(fp.c_prev == prevs + 3,
                "T31.9c: last OS repeat queued with the key-up: still dropped (one step)");
    sdl_down_gap(LV_KEY_LEFT, 0);
    sdl_keyup(LV_KEY_LEFT);
    clock_advance(500);
    ms();
    assert_true(fp.c_prev == prevs + 4, "T31.9d: ...and the next press acts");
    /* A press the watch never saw (no record of a key-up): the 450 ms
     * window decides, as before the release reset. */
    key_down(LV_KEY_LEFT);
    key_up();
    clock_advance(100);
    key_down(LV_KEY_LEFT);
    key_up();
    clock_advance(500);
    ms();
    assert_true(fp.c_prev == prevs + 5,
                "T31.9e: presses the watch didn't report: the window drops the second");
    sdl_mode(false);

    /* Held Enter on the Music tile (SDL) still doesn't act in music; after
     * its key-up, Enter acts at once. */
    tap(LV_KEY_ESC);          /* Now Playing -> the album page */
    tap(LV_KEY_ESC);          /* the grid */
    tap(LV_KEY_ESC);          /* the switcher */
    assert_true(shell_switcher_is_open(), "T31.10a: switcher open");
    int queues0 = fp.c_queue, toggles0 = fp.c_toggle;
    hold_sdl(LV_KEY_ENTER, 1500);
    settle();
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_music &&
                M.view == MUSIC_VIEW_BROWSE,
                "T31.10b: held Enter on the Music tile (SDL) closes the switcher");
    assert_true(fp.c_queue == queues0 && fp.c_toggle == toggles0,
                "T31.10c: ...and its OS repeats don't play anything in music");
    sdl_tap_gap(LV_KEY_ENTER, 100);
    settle();
    assert_true(fp.c_queue == queues0 && M.view == MUSIC_VIEW_BROWSE && M.focus_col == MUSIC_COL_SONGS,
                "T31.10d: an Enter 20 ms after the key-up acts (the album page)");
    music_teardown();
}

static void test_group30_live_tiles(void)
{
    fprintf(stderr, "\n=== Group 30: Switcher live tiles (T9) ===\n");
    avatars_setup();
    music_setup(true, 0, true);
    for (int i = 0; i < 2; i++) ui_sidebar_set_unread(i, false);

    /* ---- Closed: no tile work, no tile timers. */
    int timers_closed = timer_count();
    ms();
    cts();
    assert_true(!M.tile_built && !CT.built, "T30.1a: tiles not rendered before the switcher opens");

    open_switcher();
    assert_true(shell_switcher_is_open(), "T30.1b: switcher open");
    assert_true(timer_count() == timers_closed + 1 && switcher_clock_running(),
                "T30.1c: while open, the only extra timer is the clock (no tile timers)");
    assert_true(M.tile_built && M.tile_idle, "T30.2a: nothing loaded: Music tile shows the note face");
    assert_true(CT.built && CT.bubble && !CT.unread,
                "T30.2b: nobody unread: Chat tile shows the speech bubble");
    assert_true(app_chat.badge() == 0, "T30.2c: chat badge 0");
    ms();
    assert_true(!M.peek_visible, "T30.2d: nobody unread: no chat pill in music's header");
    assert_true(tile_layout_same("idle, Music focused+current"),
                "T30.3a: captions/faces at identical positions (Music focused and NOW)");
    snapshot("switcher-idle");
    tap(LV_KEY_RIGHT);
    assert_true(switcher_get_focus() == 1 && tile_layout_same("Chat focused"),
                "T30.3b: identical positions with focus on the non-current tile");
    tap(LV_KEY_ESC);
    assert_true(!shell_switcher_is_open() && timer_count() == timers_closed &&
                !switcher_clock_running(), "T30.1d: closed again: no timers left behind");

    /* ---- Playing, with real art. */
    open_album(AL_FSP);
    tap(LV_KEY_DOWN);
    tap(LV_KEY_ENTER);          /* Monkey Business (Fruit Salad Party: real art), Now Playing */
    fp_set_position(83000, 150000);
    settle();
    tap(LV_KEY_ESC);            /* Now Playing -> the album page (Esc there closes Now Playing) */
    tap(LV_KEY_ESC);            /* the album page -> the grid */
    open_switcher();
    assert_true(!M.tile_idle && streq(M.tile_title, "Monkey Business"),
                "T30.4a: playing: Music tile shows the track title");
    assert_true(streq(M.tile_glyph, LV_SYMBOL_PLAY), "T30.4b: playing glyph");
    assert_true(M.tile_image && !M.tile_fallback, "T30.4c: album art: the 256 px image");

    /* Changes while the switcher is open reach the tile. */
    fp_toggle();
    settle();
    assert_true(streq(M.tile_glyph, LV_SYMBOL_PAUSE), "T30.5a: paused while open: paused glyph");
    fp_toggle();
    fp_eof();                   /* next track, by itself */
    settle();
    assert_true(streq(M.tile_title, "Banana Split") && streq(M.tile_glyph, LV_SYMBOL_PLAY),
                "T30.5b: track change while open: new title, playing glyph");

    /* Chat: messages arriving while the switcher is open. */
    ui_sidebar_set_unread(0, true);                  /* Mom */
    clock_advance(20);
    cts();
    assert_true(CT.unread && !CT.bubble && streq(CT.count, "1") && CT.newest == 0,
                "T30.6a: one unread contact: count 1, Mom");
    assert_true(tile_avatar_is(0) && strstr(CT.avatar_src, "mom.png") && !CT.initial,
                "T30.6b: Mom's avatar (the sidebar's image)");
    ui_sidebar_set_unread(1, true);                  /* Dad */
    cts();
    assert_true(streq(CT.count, "2") && CT.newest == 1 && tile_avatar_is(1) &&
                streq(CT.name, "Dad"), "T30.6c: two contacts: count 2, newest sender Dad");
    ui_sidebar_set_unread(1, true);                  /* Dad again */
    ui_sidebar_set_unread(0, true);                  /* then Mom again */
    cts();
    assert_true(streq(CT.count, "2") && CT.newest == 0 && streq(CT.name, "Mom"),
                "T30.6d: contacts, not messages: still 2; newest sender Mom");
    assert_true(app_chat.badge() == 2, "T30.6e: chat badge = contacts with unread");
    ms();
    assert_true(M.peek_visible, "T30.6f: music's header shows chat's pill (someone wrote)");
    clock_advance(20);
    snapshot("switcher-live-music-and-chat");

    /* Closed: changes don't touch the tiles until the switcher opens. */
    tap(LV_KEY_ESC);            /* switcher closed: the grid */
    open_album(AL_SONGS);
    tap(LV_KEY_ENTER);          /* One (abba kids: no art), Now Playing */
    settle();
    ms();
    assert_true(streq(M.tile_title, "Banana Split"), "T30.7a: closed: the tile is not re-rendered");
    tap(LV_KEY_ESC);            /* Now Playing -> the album page */
    tap(LV_KEY_ESC);            /* -> the grid */
    open_switcher();
    assert_true(streq(M.tile_title, "One") && M.tile_fallback && !M.tile_image,
                "T30.7b: on open: new title, fallback cover (no art)");

    /* Player goes idle while open: note face. */
    fp.st = PLAYER_IDLE;
    fp.d_track = fp.d_state = true;
    settle();
    assert_true(M.tile_idle, "T30.8: nothing loaded any more: note face");

    /* ---- Opening chat clears unread only per the existing rules. */
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_ENTER);          /* zoom into chat */
    clock_advance(300);
    assert_true(shell_visible_app() == &app_chat && ui_sidebar_unread_count() == 2,
                "T30.9a: showing chat alone clears nothing (the dots stay, as before)");
    open_contact0();            /* Mom: show_chat clears her unread */
    open_switcher();
    assert_true(streq(CT.count, "1") && CT.newest == 1 && tile_avatar_is(1),
                "T30.9b: after opening Mom's chat: count 1, Dad");
    assert_true(switcher_get_current() == 1 && tile_layout_same("Chat current+focused"),
                "T30.3c: identical positions with Chat as NOW");
    tap(LV_KEY_LEFT);
    assert_true(tile_layout_same("Chat current, Music focused"),
                "T30.3d: identical positions, Music focused, Chat NOW");
    tap(LV_KEY_ESC);
    lv_group_focus_obj(ui_sidebar_get_row(1));
    tap(LV_KEY_ENTER);          /* Dad */
    ui_sidebar_set_unread(1, true);                  /* ignored below: Dad is open... */
    ui_sidebar_set_unread(1, false);                 /* ...main.c never marks the open room */
    open_switcher();
    assert_true(CT.bubble && !CT.unread && app_chat.badge() == 0,
                "T30.9c: every unread contact opened: speech bubble again");
    tap(LV_KEY_ESC);

    music_teardown();
    avatars_teardown();
}

#endif /* CHATBOX_HAVE_SQLITE */

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

#ifdef CHATBOX_HAVE_SQLITE
/* ---- Group 33: chat screen snapshot (visual check of spacing / layout) ----
 * The chat screen as the Pi shows it, through the real shell (app_chat.c's
 * layout): booted into the first contact's chat, "No contacts yet" with no
 * contacts, then the sidebar collapsed, drawn test avatars, a mix of incoming / outgoing / emoji / Greek messages, and text
 * in the textarea. Written to $TMPDIR/chatbox-snapshots/chat_*.png. Runs
 * last: it sets the avatar directory for the rest of the process. */
static void test_group33_chat_snapshot(void)
{
    fprintf(stderr, "\n=== Group 33: Chat screen snapshot ===\n");

    if (!emoji_font_get_36()) emoji_font_init("pi/src/assets/emoji");
    avatars_setup();
    shell_setup("chat");      /* the real chat layout (app_chat.c): the shelf */
    lv_timer_handler();
    assert_true(!app_chat_test_no_contacts(), "T33.0a: contacts: no \"No contacts yet\"");
    app_chat_open_first();    /* as main.c boots */
    pump_async();
    assert_true(ui_chat_is_open() && g_current_contact == 0,
                "T33.0b: boot opens the first contact's chat");
    snapshot("chat_boot");
    tap(LV_KEY_NEXT);
    assert_true(g_current_contact == 1,
                "T33.0e: after booting into a chat, Tab goes to the next contact");
    tap(LV_KEY_NEXT);
    assert_true(g_current_contact == 0, "T33.0f: and Tab again wraps to the first");
    shell_switch_to("music");
    shell_switch_to("chat");
    assert_true(ui_chat_is_open() && g_current_contact == 0,
                "T33.0c: back from music, the same chat");

    /* No contacts: "No contacts yet", and booting opens nothing. */
    {
        int saved = g_cfg.contact_count;
        g_cfg.contact_count = 0;
        shell_setup("chat");
        app_chat_open_first();
        lv_timer_handler();
        assert_true(app_chat_test_no_contacts() && !ui_chat_is_open(),
                    "T33.0d: no contacts: \"No contacts yet\", no chat opened");
        snapshot("chat_no_contacts");
        g_cfg.contact_count = saved;
        shell_setup("chat");
        lv_timer_handler();
    }
    show_chat(1);
    ui_chat_set_language(LANG_GREEK);
    pump_async();

    ui_chat_add_message("Καλημέρα! Τι κάνεις;", false);
    ui_chat_add_message("love you!", true);
    ui_chat_add_message("\xF0\x9F\x92\x96\xF0\x9F\x92\x96\xF0\x9F\x92\x96\xF0\x9F\x92\x96", true);
    ui_chat_add_message("Σ’ αγαπάω \xF0\x9F\xA5\xB0\xF0\x9F\x98\x98\xF0\x9F\x98\x8D", false);
    ui_chat_add_message("Θέλεις καφεδάκι; We could go to the park after school if the weather is nice", false);
    ui_chat_add_message("NE", true);
    ui_chat_add_message("ok", true);
    type_utf8("ηγ gyp Ώ");
    pump_async();
    /* add_message's scroll is animated; jump to the end for the picture */
    lv_obj_scroll_to_y(ui_chat_get_msg_cont(), LV_COORD_MAX, LV_ANIM_OFF);
    snapshot("chat_screen");

    lv_group_focus_obj(ui_chat_get_textarea());
    press_key(LV_KEY_MENU);
    pump_async();
    assert_not_null(ui_chat_get_emoji_panel(), "T33.1: emoji panel open");
    snapshot("chat_emoji_panel");
    press_key(LV_KEY_MENU);
    shell_teardown();
    avatars_teardown();
}
#endif


#ifdef CHATBOX_HAVE_SQLITE
/* ---- Group 34: Photos (docs/photos.md) --------------------------------------
 * The Photos app through the real shell with three apps: the grid (4
 * across, newest first), the viewer (whole photo, letterboxed), delete with
 * the confirm sticker, the screen-sleep override while the viewer is up,
 * the third switcher tile and chat's photo bubbles. Test photos are painted
 * JPEGs / PNGs (test_photo_fixture.c) in $TMPDIR/test_keyboard_photos.
 * Snapshots: photos-*.png, switcher-three-apps.png, chat_photo_bubbles*.png. */

static photos_test_state_t P;
static void ps(void) { ui_photos_test_state(&P); }
static char g_pdir[600];
static photo_lib_t *g_plib = NULL;

static void pdir_clear(void)
{
    DIR *d = opendir(g_pdir);
    if (!d) return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
            continue;
        snprintf(p, sizeof(p), "%s/%s", g_pdir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

static void photo_at(const char *name, time_t t)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", g_pdir, name);
    struct timeval tv[2] = { { t, 0 }, { t, 0 } };
    utimes(p, tv);
}

/* Seven photos, newest first:
 *   0 beach.jpg      4:3 landscape     4 broken.jpg   not really a JPEG
 *   1 pano.jpg       3:1 panorama      5 park.jpg     4:3
 *   2 portrait.jpg   3:4, stored sideways (EXIF 6)    6 snow.jpg  4:3
 *   3 garden.png     4:3 PNG */
#define PHOTO_T0 1763200000   /* 15 Nov 2025 */
static bool photos_fixture(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_pdir, sizeof(g_pdir), "%s%stest_keyboard_photos", (tmp && *tmp) ? tmp : "/tmp",
             (tmp && *tmp && tmp[strlen(tmp) - 1] == '/') ? "" : "/");
    mkdir(g_pdir, 0755);
    pdir_clear();
    char p[1024];
    bool ok = true;
#define PF(name, call, age) do { snprintf(p, sizeof(p), "%s/%s", g_pdir, name); ok &= call; \
                                 photo_at(name, PHOTO_T0 - (age) * 86400); } while (0)
    PF("beach.jpg",    photo_fixture_jpeg(p, 1600, 1200, 1, 1), 0);
    PF("pano.jpg",     photo_fixture_jpeg(p, 1800, 600, 2, 1), 1);
    PF("portrait.jpg", photo_fixture_jpeg(p, 900, 1200, 3, 6), 2);
    PF("garden.png",   photo_fixture_png(p, 800, 600, 4, false), 3);
    snprintf(p, sizeof(p), "%s/broken.jpg", g_pdir);
    FILE *f = fopen(p, "wb");
    if (f) { fputs("\xFF\xD8\xFF not really a photo", f); fclose(f); }
    photo_at("broken.jpg", PHOTO_T0 - 4 * 86400);
    PF("park.jpg",     photo_fixture_jpeg(p, 1600, 1200, 5, 1), 5);
    PF("snow.jpg",     photo_fixture_jpeg(p, 1600, 1200, 6, 1), 6);
#undef PF
    return ok;
}

static bool photos_focus_ok(void)
{
    lv_group_t *g = app_photos.group();
    return lv_indev_get_group(g_test_indev) == g && lv_group_get_focused(g) == app_photos_get_sink();
}

static void photos_setup(void)
{
    g_plib = photo_lib_open(g_pdir);
    app_photos_configure(g_plib);
    g_with_photos = true;
    shell_setup("photos");
    clock_advance(100);
    ps();
}

static void photos_teardown(void)
{
    shell_teardown();
    g_with_photos = false;
    app_photos_configure(NULL);
    photo_lib_close(g_plib);
    g_plib = NULL;
}

/* From the grid: photo i selected (row by row, then across). */
static void photo_go(int i)
{
    ps();
    for (int guard = 0; guard < 50 && P.sel != i; guard++) {
        if (P.sel / 4 != i / 4) tap(P.sel / 4 < i / 4 ? LV_KEY_DOWN : LV_KEY_UP);
        else tap(P.sel < i ? LV_KEY_RIGHT : LV_KEY_LEFT);
        ps();
    }
}

static bool file_in_pdir(const char *name)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", g_pdir, name);
    return access(p, F_OK) == 0;
}

/* Fake media fetch for chat's photos: keeps the callback (a download in flight). */
static photo_inbox_media_cb_t g_kb_fetch_cb;
static void *g_kb_fetch_ud;
static void kb_fetch(const char *mxc, bool legacy, size_t max, photo_inbox_media_cb_t cb, void *ud)
{
    (void)mxc; (void)legacy; (void)max;
    g_kb_fetch_cb = cb;
    g_kb_fetch_ud = ud;
}

static cJSON *kb_image_event(const char *eid, const char *mime, double ts, int w, int h)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "type", "m.room.message");
    cJSON_AddStringToObject(e, "event_id", eid);
    cJSON_AddNumberToObject(e, "origin_server_ts", ts);
    cJSON *c = cJSON_AddObjectToObject(e, "content");
    cJSON_AddStringToObject(c, "msgtype", "m.image");
    cJSON_AddStringToObject(c, "body", "IMG_0001.jpg");
    cJSON_AddStringToObject(c, "url", "mxc://localhost/abc");
    cJSON *i = cJSON_AddObjectToObject(c, "info");
    cJSON_AddStringToObject(i, "mimetype", mime);
    cJSON_AddNumberToObject(i, "w", w);
    cJSON_AddNumberToObject(i, "h", h);
    return e;
}

static void test_group34_photos(void)
{
    fprintf(stderr, "\n=== Group 34: Photos ===\n");
    bool pics = photos_fixture();
    if (!pics) fprintf(stderr, "  (no libjpeg/libpng: picture checks skipped)\n");
    photos_setup();

    /* ---- Boot into Photos: the grid. */
    assert_true(shell_visible_app() == &app_photos && photos_focus_ok(),
                "T34.1a: Photos visible, keys on its sink");
    assert_true(P.view == PHOTOS_VIEW_GRID && P.grid_visible && !P.viewer_visible && !P.empty_visible,
                "T34.1b: the grid");
    assert_true(P.count == 7 && P.sel == 0 && streq(P.head_sub, "7 photos"),
                "T34.1c: seven photos, the newest selected, \"7 photos\"");
    assert_true(P.sel_card_focused, "T34.1d: the selected card wears the focus look");
    assert_true(streq(P.sel_caption, "15 November 2025"), "T34.1e: cards show their date");
    assert_true(P.cards_bound == 7, "T34.1f: seven cards bound");
    clock_advance(400);                 /* the loader: one card per tick */
    ui_photos_test_flush();
    ps();
    if (pics)
        assert_true(P.cards_image == 6 && P.cards_failed == 1,
                    "T34.1g: six pictures, the broken file shows the can't-show glyph");
    assert_true(ui_photos_key_ctx() == KEY_CTX_PHOTOS, "T34.1h: key context photos");
    snapshot("photos-grid");

    /* ---- Grid keys: arrows move, edges bounce, no wrap. */
    tap(LV_KEY_RIGHT); ps();
    assert_true(P.sel == 1, "T34.2a: → next card");
    tap(LV_KEY_LEFT); ps();
    assert_true(P.sel == 0, "T34.2b: ← back");
    int b = P.bounces;
    tap(LV_KEY_LEFT); ps();
    assert_true(P.sel == 0 && P.bounces == b + 1 && P.last_bounce == PHOTOS_BOUNCE_LEFT,
                "T34.2c: ← at the left edge bounces");
    tap(LV_KEY_UP); ps();
    assert_true(P.sel == 0 && P.last_bounce == PHOTOS_BOUNCE_UP, "T34.2d: ↑ on the top row bounces");
    tap(LV_KEY_DOWN); ps();
    assert_true(P.sel == 4, "T34.2e: ↓ one row down");
    tap(LV_KEY_DOWN); ps();
    assert_true(P.sel == 4 && P.last_bounce == PHOTOS_BOUNCE_DOWN, "T34.2f: ↓ on the last row bounces");
    tap(LV_KEY_UP); tap(LV_KEY_RIGHT); tap(LV_KEY_RIGHT); tap(LV_KEY_RIGHT); ps();
    assert_true(P.sel == 3, "T34.2g: across the top row");
    tap(LV_KEY_RIGHT); ps();
    assert_true(P.sel == 3 && P.last_bounce == PHOTOS_BOUNCE_RIGHT, "T34.2h: → at the right edge bounces");
    tap(LV_KEY_DOWN); ps();
    assert_true(P.sel == 6, "T34.2i: ↓ into a shorter last row lands on its last card");
    tap(KF_KEY_SPACE); tap(LV_KEY_PG_UP); tap(LV_KEY_MENU); tap('a'); ps();
    assert_true(P.sel == 6 && P.view == PHOTOS_VIEW_GRID && !shell_switcher_is_open(),
                "T34.2j: Space, PgUp, Win, letters do nothing");
    photo_go(0);

    /* ---- The viewer. */
    tap(LV_KEY_ENTER); ps();
    assert_true(P.view == PHOTOS_VIEW_VIEWER && P.viewer_visible && !P.grid_visible,
                "T34.3a: Enter opens the viewer");
    assert_true(streq(P.viewer_count, "1 of 7") && streq(P.viewer_title, "15 November 2025"),
                "T34.3b: \"1 of 7\" and the date");
    if (pics) assert_true(P.viewer_image && P.viewer_img_w == 789 && P.viewer_img_h == 592,
                          "T34.3c: a 4:3 photo scaled to fit: 789×592");
    assert_true(P.arrow_left_dim && !P.arrow_right_dim, "T34.3d: first photo: ← pale, → not");
    assert_true(photos_focus_ok(), "T34.3e: keys still on the sink");
    snapshot("photos-viewer");
    tap(LV_KEY_RIGHT); ps();
    assert_true(streq(P.viewer_count, "2 of 7"), "T34.3f: → next photo");
    if (pics) assert_true(P.viewer_img_w == 1168 && P.viewer_img_h == 389,
                          "T34.3g: a 3:1 panorama is letterboxed: 1168×389");
    snapshot("photos-viewer-panorama");
    tap(LV_KEY_RIGHT); ps();
    if (pics) assert_true(P.viewer_img_w == 444 && P.viewer_img_h == 592,
                          "T34.3h: the sideways-stored portrait shows upright: 444×592");
    snapshot("photos-viewer-portrait");
    tap(LV_KEY_RIGHT); tap(LV_KEY_RIGHT); ps();
    assert_true(streq(P.viewer_count, "5 of 7") && P.viewer_failed && !P.viewer_image,
                "T34.3i: a file that doesn't decode: the can't-show placeholder");
    snapshot("photos-viewer-cant-show");
    tap(LV_KEY_LEFT); ps();
    assert_true(streq(P.viewer_count, "4 of 7") && (!pics || P.viewer_image),
                "T34.3j: ← previous photo");
    hold_pi(LV_KEY_RIGHT, 1500); ps();
    assert_true(streq(P.viewer_count, "5 of 7"), "T34.3k: a held → moves one photo (one-shot)");
    tap(LV_KEY_RIGHT); tap(LV_KEY_RIGHT); ps();
    assert_true(streq(P.viewer_count, "7 of 7") && !P.arrow_left_dim && P.arrow_right_dim,
                "T34.3l: last photo: → pale");
    b = P.bounces;
    tap(LV_KEY_RIGHT); ps();
    assert_true(streq(P.viewer_count, "7 of 7") && P.bounces == b + 1 &&
                P.last_bounce == PHOTOS_BOUNCE_RIGHT, "T34.3m: → at the end bounces");
    tap(LV_KEY_UP); tap(LV_KEY_DOWN); tap(LV_KEY_ENTER); ps();
    assert_true(P.view == PHOTOS_VIEW_VIEWER && streq(P.viewer_count, "7 of 7"),
                "T34.3n: ↑ ↓ Enter do nothing in the viewer");
    tap(LV_KEY_ESC); ps();
    assert_true(P.view == PHOTOS_VIEW_GRID && P.sel == 6 && !shell_switcher_is_open(),
                "T34.3o: Esc: back to the grid, on the photo that was open");
    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T34.3p: Esc on the grid opens the switcher");
    tap(LV_KEY_ESC);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_photos,
                "T34.3q: and Esc closes it again");

    /* ---- Delete from the grid, with the confirm sticker. */
    photo_go(4);                                   /* broken.jpg */
    tap(LV_KEY_DEL); ps();
    assert_true(P.confirm_open && ui_photos_confirm_open(), "T34.4a: Delete asks first");
    assert_true(!P.sel_card_focused, "T34.4b: the card shows chosen, not focused, under the sticker");
    snapshot("photos-confirm");
    tap(LV_KEY_LEFT); tap(LV_KEY_DOWN); tap(LV_KEY_DEL); tap(KF_KEY_SPACE); ps();
    assert_true(P.confirm_open && P.sel == 4, "T34.4c: other keys do nothing while it asks");
    tap(LV_KEY_ESC); ps();
    assert_true(!P.confirm_open && P.count == 7 && file_in_pdir("broken.jpg") &&
                !shell_switcher_is_open(), "T34.4d: Esc keeps the photo (and doesn't open the switcher)");
    hold_pi(LV_KEY_DEL, 1200); ps();
    assert_true(P.confirm_open, "T34.4e: a held Delete opens the sticker once");
    tap(LV_KEY_ENTER); ps();
    assert_true(!P.confirm_open && P.count == 6 && !file_in_pdir("broken.jpg"),
                "T34.4f: Enter deletes the file");
    assert_true(P.sel == 4 && streq(P.head_sub, "6 photos"),
                "T34.4g: the next photo takes its place, \"6 photos\"");

    /* ---- Delete from the viewer. */
    photo_go(5);                                   /* snow.jpg, the last */
    tap(LV_KEY_ENTER);
    tap(LV_KEY_DEL);
    tap(LV_KEY_ENTER); ps();
    assert_true(P.view == PHOTOS_VIEW_VIEWER && !file_in_pdir("snow.jpg") &&
                streq(P.viewer_count, "5 of 5"),
                "T34.5a: deleting the last photo shows the one before it");
    tap(LV_KEY_ESC);
    photo_go(1);                                   /* pano.jpg */
    tap(LV_KEY_ENTER);
    tap(LV_KEY_DEL);
    tap(LV_KEY_ENTER); ps();
    assert_true(!file_in_pdir("pano.jpg") && streq(P.viewer_count, "2 of 4") &&
                (!pics || P.viewer_img_w == 444), "T34.5b: deleting another shows the next (the portrait)");

    /* ---- The library changes under the viewer: stays on the same photo. */
    {
        char p[1024];
        snprintf(p, sizeof(p), "%s/new.jpg", g_pdir);
        photo_fixture_jpeg(p, 1600, 1200, 9, 1);
        if (!pics) { FILE *f = fopen(p, "wb"); if (f) { fputs("x", f); fclose(f); } }
        photo_at("new.jpg", PHOTO_T0 + 86400);
        int dec = P.viewer_decodes;
        app_photos_poll_library(true);
        ps();
        assert_true(P.count == 5 && streq(P.viewer_count, "3 of 5") && (!pics || P.viewer_img_w == 444),
                    "T34.6a: a new photo arrives: the viewer stays on the portrait (now 3 of 5)");
        assert_true(P.viewer_decodes == dec, "T34.6b: ...without decoding it again");
        if (pics) {
            /* The open photo itself changes (same name and time, new bytes). */
            snprintf(p, sizeof(p), "%s/portrait.jpg", g_pdir);
            photo_fixture_jpeg(p, 900, 1200, 8, 6);
            photo_at("portrait.jpg", PHOTO_T0 - 2 * 86400);
            app_photos_poll_library(true);
            ps();
            assert_true(P.viewer_decodes == dec + 1 && streq(P.viewer_count, "3 of 5") &&
                        P.viewer_img_w == 444, "T34.6c: the open photo changed on disk: decoded again");
        }
    }

    /* ---- Screen sleep: the viewer stays awake longer, only while it's up. */
    display_sleep_init(1, 2);
    display_sleep_set_lock_override(app_photos_viewer_showing, 3);
    clock_advance(1100);
    display_sleep_force_check();
    assert_true(app_photos_viewer_showing() && display_sleep_get_state() == 0,
                "T34.7a: viewer up: still awake after the normal 1 s");
    clock_advance(2000);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1, "T34.7b: ...locks after the viewer's 3 s");
    tap(LV_KEY_LEFT);
    display_sleep_force_check();
    ps();
    assert_true(display_sleep_get_state() == 0 && streq(P.viewer_count, "3 of 5"),
                "T34.7c: the waking key wakes, nothing else");
    tap(LV_KEY_ESC); ps();
    assert_true(P.view == PHOTOS_VIEW_GRID && !app_photos_viewer_showing(), "T34.7d: back on the grid");
    clock_advance(1100);
    display_sleep_force_check();
    assert_true(display_sleep_get_state() == 1, "T34.7e: the grid sleeps after the normal 1 s again");
    tap(LV_KEY_ENTER);                             /* wakes only */
    display_sleep_force_check();
    ps();
    assert_true(display_sleep_get_state() == 0 && P.view == PHOTOS_VIEW_GRID,
                "T34.7f: woken, still the grid");
    tap(LV_KEY_ENTER); ps();
    assert_true(P.view == PHOTOS_VIEW_VIEWER, "T34.7g: viewer up");
    shell_open_switcher();
    clock_advance(1100);
    display_sleep_force_check();
    assert_true(!app_photos_viewer_showing() && display_sleep_get_state() == 1,
                "T34.7h: the switcher over the viewer sleeps like everything else");
    tap(LV_KEY_ENTER);                             /* wake */
    display_sleep_force_check();
    shell_close_switcher();
    display_sleep_set_lock_override(NULL, 0);
    display_sleep_deinit();
    tap(LV_KEY_ESC); ps();
    assert_true(P.view == PHOTOS_VIEW_GRID, "T34.7i: grid");

    /* ---- The photo being asked about vanishes (removed by hand): the
     * question goes away, Enter can't delete a neighbour nobody chose. */
    {
        static const char *const rest[] = { "new.jpg", "beach.jpg", "portrait.jpg", "garden.png", "park.jpg" };
        char victim[PHOTO_NAME_MAX], p[1024];
        tap(LV_KEY_DEL); ps();
        snprintf(victim, sizeof(victim), "%s", photo_lib_get(g_plib, P.sel)->name);
        snprintf(p, sizeof(p), "%s/%s", g_pdir, victim);
        unlink(p);
        app_photos_poll_library(true);
        ps();
        assert_true(!P.confirm_open && P.count == 4 && P.view == PHOTOS_VIEW_GRID,
                    "T34.12a: grid: the photo asked about is gone: no more question");
        tap(LV_KEY_ENTER); ps();
        int left = 0;
        for (int i = 0; i < 5; i++) left += file_in_pdir(rest[i]);
        assert_true(left == 4 && P.count == 4 && P.view == PHOTOS_VIEW_VIEWER,
                    "T34.12b: ...so Enter opens a photo, it deletes nothing");
        tap(LV_KEY_DEL); ps();
        snprintf(victim, sizeof(victim), "%s", photo_lib_get(g_plib, P.sel)->name);
        snprintf(p, sizeof(p), "%s/%s", g_pdir, victim);
        unlink(p);
        app_photos_poll_library(true);
        ps();
        assert_true(!P.confirm_open && P.count == 3 && P.view == PHOTOS_VIEW_VIEWER,
                    "T34.12c: viewer: the same");
        tap(LV_KEY_ENTER); ps();
        left = 0;
        for (int i = 0; i < 5; i++) left += file_in_pdir(rest[i]);
        assert_true(left == 3 && P.count == 3, "T34.12d: ...and Enter deletes nothing");
        tap(LV_KEY_ESC); ps();

        /* The file goes while the question is up, before any rescan: the
         * delete fails, the grid shows the library as it is now. */
        photo_go(1);                               /* beach.jpg, between new and park */
        tap(LV_KEY_DEL);
        snprintf(p, sizeof(p), "%s/beach.jpg", g_pdir);
        unlink(p);
        int bnc = P.bounces;
        tap(LV_KEY_ENTER); ps();
        assert_true(P.bounces == bnc + 1 && !P.confirm_open, "T34.12e: the delete fails: a bounce");
        assert_true(P.count == 2 && P.sel == 1 && P.cards_bound == 2 &&
                    streq(P.sel_caption, "10 November 2025") && streq(P.head_sub, "2 photos"),
                    "T34.12f: ...and the cards show the photos now there (park.jpg in its place)");
        tap(LV_KEY_DEL);                           /* park.jpg, the last */
        snprintf(p, sizeof(p), "%s/park.jpg", g_pdir);
        unlink(p);
        tap(LV_KEY_ENTER); ps();
        assert_true(P.count == 1 && P.sel == 0 && streq(P.sel_caption, "16 November 2025") &&
                    P.sel_card_focused, "T34.12g: the last one vanished: the selection moves onto the one left");
    }

    /* ---- Chat saves a backlog of photos (the kiosk was off): the folder is
     * listed once for all of them, on the next poll, not once per photo. */
    {
        unsigned scans = photo_lib_test_scans(g_plib);
        int before = P.count;
        for (int i = 0; i < 5; i++) {
            char name[32], p[1024];
            snprintf(name, sizeof(name), "backlog%d.jpg", i);
            snprintf(p, sizeof(p), "%s/%s", g_pdir, name);
            FILE *f = fopen(p, "wb");
            if (f) { fputs("\xFF\xD8\xFF backlog", f); fclose(f); }
            photo_at(name, PHOTO_T0 - (10 + i) * 86400);
            app_photos_photo_saved();
        }
        assert_true(photo_lib_test_scans(g_plib) == scans,
                    "T34.14a: five photos saved: no rescan per photo");
        app_photos_poll_library(false);
        ps();
        assert_true(photo_lib_test_scans(g_plib) == scans + 1 && P.count == before + 5,
                    "T34.14b: the next poll lists the folder once and finds all five");
        app_photos_poll_library(false);
        assert_true(photo_lib_test_scans(g_plib) == scans + 1,
                    "T34.14c: the poll after that: nothing changed, no rescan");
    }

    /* ---- Delete everything: "No photos yet". */
    for (int guard = 0; guard < 10 && P.count > 0; guard++) {
        tap(LV_KEY_DEL);
        tap(LV_KEY_ENTER);
        ps();
    }
    assert_true(P.count == 0 && P.view == PHOTOS_VIEW_EMPTY && P.empty_visible && !P.grid_visible,
                "T34.8a: the last photo deleted: \"No photos yet\"");
    tap(LV_KEY_DEL); tap(LV_KEY_ENTER); tap(LV_KEY_RIGHT); ps();
    assert_true(P.view == PHOTOS_VIEW_EMPTY && !P.confirm_open, "T34.8b: keys do nothing there");
    snapshot("photos-empty");
    tap(LV_KEY_ESC);
    assert_true(shell_switcher_is_open(), "T34.8c: Esc opens the switcher");
    tap(LV_KEY_ESC);
    photos_teardown();

    /* ---- The switcher with three apps (music playing, a friend wrote). */
    photos_fixture();
    g_plib = photo_lib_open(g_pdir);
    app_photos_configure(g_plib);
    g_with_photos = true;
    avatars_setup();
    music_setup(true, 0, true);
    open_album(AL_FSP);
    tap(LV_KEY_DOWN);
    tap(LV_KEY_ENTER);                             /* Monkey Business, Now Playing */
    settle();
    tap(LV_KEY_ESC);
    tap(LV_KEY_ESC);
    ui_sidebar_set_unread(0, true);
    open_switcher();
    ps();
    assert_true(switcher_get_tile(2) != NULL && switcher_get_tile(3) == NULL,
                "T34.9a: three tiles");
    assert_true(P.tile_built && streq(P.tile_label, "7 photos") && (!pics || P.tile_image),
                "T34.9b: the Photos tile: the newest photo and \"7 photos\"");
    {
        /* Each tile wears its app's name and colour. */
        int named = 0;
        for (int i = 0; i < 3; i++) {
            const char *cap = lv_label_get_text(switcher_get_caption(i));
            lv_color_t bg = lv_obj_get_style_bg_color(switcher_get_tile(i), 0);
            lv_color_t fg = lv_obj_get_style_text_color(switcher_get_caption(i), 0);
            if ((streq(cap, "Chat") && lv_color_eq(bg, UI_C_CHAT) && lv_color_eq(fg, UI_C_ON_CHAT)) ||
                (streq(cap, "Music") && lv_color_eq(bg, UI_C_MUSIC) && lv_color_eq(fg, UI_C_ON_MUSIC)) ||
                (streq(cap, "Photos") && lv_color_eq(bg, UI_C_PHOTOS) && lv_color_eq(fg, UI_C_ON_PHOTOS)))
                named |= 1 << i;
        }
        assert_true(named == 7, "T34.9i: Chat, Music, Photos: each tile its app's name and colours");
    }
    assert_true(lv_obj_get_width(switcher_get_tile(0)) == lv_obj_get_width(switcher_get_tile(2)) &&
                lv_obj_get_height(switcher_get_tile(0)) > lv_obj_get_width(switcher_get_tile(0)),
                "T34.9c: three equal upright tiles");
    lv_area_t a2;
    lv_obj_get_coords(switcher_get_tile(2), &a2);
    assert_true(a2.x2 < 1280 - 40, "T34.9d: the third tile is on screen");
    clock_advance(20);
    snapshot("switcher-three-apps");
    tap(LV_KEY_RIGHT);
    tap(LV_KEY_RIGHT);
    assert_true(switcher_get_focus() == 2, "T34.9e: → → focuses Photos");
    int sh = switcher_get_shake_count();
    tap(LV_KEY_RIGHT);
    assert_true(switcher_get_focus() == 2 && switcher_get_shake_count() == sh + 1,
                "T34.9f: → past Photos shakes");
    tap(LV_KEY_ENTER);
    clock_advance(400);
    assert_true(!shell_switcher_is_open() && shell_visible_app() == &app_photos,
                "T34.9g: Enter on the Photos tile opens Photos");
    ps();
    assert_true(P.view == PHOTOS_VIEW_GRID && photos_focus_ok(), "T34.9h: on its grid, keys on its sink");
    music_teardown();
    g_with_photos = false;
    app_photos_configure(NULL);
    photo_lib_close(g_plib);
    g_plib = NULL;

    /* ---- Chat shows photos: saved, on its way, refused. */
    photos_fixture();
    photo_inbox_init(g_pdir, NULL, NULL);
    photo_inbox_test_set_fetch(kb_fetch);
    shell_setup("chat");
    lv_timer_handler();
    show_chat(1);
    pump_async();
    ui_chat_add_message("Look what we found at the beach!", false);
    {
        char src[1024];
        size_t len = 0;
        snprintf(src, sizeof(src), "%s/beach.jpg", g_pdir);
        FILE *f = fopen(src, "rb");
        unsigned char *buf = NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            len = (size_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            buf = malloc(len);
            if (buf && fread(buf, 1, len, f) != len) len = 0;
            fclose(f);
        }
        if (buf && len) photo_inbox_save("$saved:localhost", 1763300000000LL, buf, len, NULL, 0);
        free(buf);
    }
    ui_chat_add_photo("$saved:localhost", 1763300000000LL, 1600, 1200, false);
    cJSON *ev = kb_image_event("$coming:localhost", "image/jpeg", 1763300060000.0, 1200, 1600);
    photo_inbox_offer(ev);
    photo_inbox_tick(lv_tick_get());
    ui_chat_add_photo("$coming:localhost", 1763300060000LL, 1200, 1600, false);
    cJSON_Delete(ev);
    ev = kb_image_event("$gif:localhost", "image/gif", 1763300120000.0, 400, 300);
    photo_inbox_offer(ev);
    ui_chat_add_photo("$gif:localhost", 1763300120000LL, 400, 300, false);
    cJSON_Delete(ev);
    ui_chat_add_message("wow!!", true);
    pump_async();
    ui_chat_test_flush_photos();
    int with_pic = 0;
    int nb = ui_chat_test_photo_bubbles(&with_pic);
    assert_true(nb == 3, "T34.10a: three photo bubbles");
    if (pics) assert_true(with_pic == 1, "T34.10b: the saved one shows its picture, the others wait / refuse");
    assert_true(photo_inbox_status("$coming:localhost", 1763300060000LL, NULL, 0) == PHOTO_ST_LOADING,
                "T34.10c: the second is still on its way");
    lv_obj_scroll_to_y(ui_chat_get_msg_cont(), LV_COORD_MAX, LV_ANIM_OFF);
    snapshot("chat_photo_bubbles");
    /* It arrives. */
    if (g_kb_fetch_cb) {
        char src[1024];
        snprintf(src, sizeof(src), "%s/portrait.jpg", g_pdir);
        FILE *f = fopen(src, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size_t len = (size_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char *buf = malloc(len);
            if (buf && fread(buf, 1, len, f) == len) {
                photo_inbox_test_set_fetch(NULL);
                /* The inbox was started without a listener: tell chat as main.c would. */
                g_kb_fetch_cb(buf, len, 200, 0, g_kb_fetch_ud);
                ui_chat_photo_changed("$coming:localhost");
            }
            free(buf);
            fclose(f);
        }
    }
    ui_chat_test_flush_photos();
    nb = ui_chat_test_photo_bubbles(&with_pic);
    if (pics) assert_true(with_pic == 2, "T34.10d: the download lands: its bubble shows the picture");
    assert_true(photo_inbox_status("$coming:localhost", 1763300060000LL, NULL, 0) == PHOTO_ST_READY,
                "T34.10e: ...and it is saved in the photo folder");
    lv_obj_scroll_to_y(ui_chat_get_msg_cont(), LV_COORD_MAX, LV_ANIM_OFF);
    snapshot("chat_photo_bubbles_arrived");
    /* Deleting the file in Photos doesn't change a bubble that shows it. */
    {
        char path[1024];
        if (photo_inbox_status("$saved:localhost", 1763300000000LL, path, sizeof(path)) == PHOTO_ST_READY)
            unlink(path);
        ui_chat_photo_changed("$saved:localhost");
        ui_chat_test_photo_bubbles(&with_pic);
        if (pics) assert_true(with_pic == 2, "T34.10f: a deleted photo stays in its chat bubble");
    }
    /* Pictures that load during the slide to a new bubble keep the list at
     * the bottom: the events give no size, so the 4:3 boxes become portrait
     * ones (1536x2048) when decoded. Scrolled up, a late picture leaves the
     * list where it is. */
    if (pics) {
        char src[1024];
        snprintf(src, sizeof(src), "%s/tall.jpg", g_pdir);
        unsigned char *buf = NULL;
        size_t len = 0;
        FILE *f = photo_fixture_jpeg(src, 1536, 2048, 7, 1) ? fopen(src, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            len = (size_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            buf = malloc(len);
            if (buf && fread(buf, 1, len, f) != len) len = 0;
            fclose(f);
        }
        unlink(src);
        if (buf && len) {
            photo_inbox_save("$tall1:localhost", 1763400000000LL, buf, len, NULL, 0);
            photo_inbox_save("$tall2:localhost", 1763400060000LL, buf, len, NULL, 0);
        }
        show_chat(0);
        pump_async();
        ui_chat_add_message("Cleo!", false);
        ui_chat_add_message("\xE2\x9D\xA4", false);
        ui_chat_add_photo("$tall1:localhost", 1763400000000LL, 0, 0, false);
        ui_chat_add_photo("$tall2:localhost", 1763400060000LL, 0, 0, false);
        clock_advance(1000);
        lv_obj_t *mc = ui_chat_get_msg_cont();
        lv_obj_t *box = lv_obj_get_child(lv_obj_get_child(mc, -1), 0);
        int bh = (int)lv_obj_get_height(box);
        assert_true(bh == 300 && (int)lv_obj_get_width(box) == 225,
                    "T34.11a: a 1536x2048 photo's bubble is 225x300 once decoded");
        assert_true(bh <= (int)lv_obj_get_content_height(mc),
                    "T34.11b: ...which fits in the message list");
        assert_true(lv_obj_get_scroll_bottom(mc) <= 0,
                    "T34.11c: pictures decoded mid-slide: the list ends at the bottom");
        snapshot("chat_photo_bubbles_tall");
        /* Scrolled up, the next picture arrives late. */
        ui_chat_add_photo("$tall3:localhost", 1763400120000LL, 0, 0, false);
        clock_advance(1000);
        lv_obj_scroll_to_y(mc, 0, LV_ANIM_OFF);
        if (buf && len) photo_inbox_save("$tall3:localhost", 1763400120000LL, buf, len, NULL, 0);
        ui_chat_photo_changed("$tall3:localhost");
        clock_advance(500);
        ui_chat_test_photo_bubbles(&with_pic);
        assert_true(with_pic == 3 && lv_obj_get_scroll_y(mc) == 0,
                    "T34.11d: scrolled up, a late picture doesn't move the list");

        /* Photos keep coming in a chat that is never evicted (three
         * contacts fit the panel cache): far more than the decode budget
         * holds. The newest always shows its picture; older ones off
         * screen give theirs back, and get it again when scrolled to. */
        lv_obj_scroll_to_y(mc, LV_COORD_MAX, LV_ANIM_OFF);
        int shown_new = 0;
        char id[64];
        for (int i = 0; i < 48; i++) {
            snprintf(id, sizeof(id), "$many%d:localhost", i);
            if (buf && len) photo_inbox_save(id, 1763500000000LL + i * 60000LL, buf, len, NULL, 0);
            ui_chat_add_photo(id, 1763500000000LL + i * 60000LL, 1536, 2048, false);
            clock_advance(400);
            ui_chat_test_flush_photos();
            shown_new += ui_chat_test_photo_shows(id);
        }
        assert_true(shown_new == 48, "T34.13a: 48 photos in a row: each new one shows its picture");
        assert_true(ui_chat_test_photo_bytes() <= 8u * 1024u * 1024u,
                    "T34.13b: ...within the 8 MB budget");
        assert_true(!ui_chat_test_photo_shows("$tall1:localhost"),
                    "T34.13c: the oldest, off screen, gave its picture back");
        lv_obj_scroll_to_y(mc, 0, LV_ANIM_OFF);
        clock_advance(400);
        ui_chat_test_flush_photos();
        assert_true(ui_chat_test_photo_shows("$tall1:localhost"),
                    "T34.13d: scrolled up to it: it shows its picture again");
        lv_obj_scroll_to_y(mc, LV_COORD_MAX, LV_ANIM_OFF);
        clock_advance(400);
        ui_chat_test_flush_photos();
        assert_true(ui_chat_test_photo_shows("$many47:localhost") &&
                    ui_chat_test_photo_bytes() <= 8u * 1024u * 1024u,
                    "T34.13e: back at the bottom: the newest shows its picture");
        snapshot("chat_photo_bubbles_many");
        free(buf);
    }
    shell_teardown();
    photo_inbox_test_set_fetch(NULL);
    photo_inbox_deinit();
    pdir_clear();
    rmdir(g_pdir);
    avatars_teardown();
}
#endif

int main(int argc, char *argv[])
{
    const char *cfg_path = (argc > 1) ? argv[1] : "test/fixtures/chatbox.conf";
    if (config_parse(cfg_path, &g_cfg) != 0) {
        if (config_parse("/app/test/fixtures/chatbox.conf", &g_cfg) != 0) {
            fprintf(stderr, "Failed to parse config\n");
            return 1;
        }
    }

    if (g_cfg.contact_count < 2) {
        fprintf(stderr, "Need at least 2 contacts in config for tests\n");
        return 1;
    }

    /* Init LVGL with dummy display */
    lv_init();
    lv_tick_set_cb(test_tick_cb);

    static uint8_t buf[1280 * 10 * 4];
    lv_display_t *disp = lv_display_create(1280, 800);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, dummy_flush_cb);

    /* Create virtual keypad indev for ENTER simulation */
    g_test_indev = lv_indev_create();
    lv_indev_set_type(g_test_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(g_test_indev, test_keypad_read_cb);

    lv_timer_handler();

    fprintf(stderr, "=== Keyboard Navigation Tests ===\n");
    fprintf(stderr, "Config: %d contacts\n", g_cfg.contact_count);

    test_group1_sidebar_navigation();
    test_group2_chat_navigation();
    test_group3_emoji_open_close();
    test_group4_tab_with_emoji();
    test_group5_emoji_grid_nav();
    test_group10_emoji_pages();
    test_group6_contact_switching();
    test_group7_unread_dots();
    test_group8_new_divider();
    test_group11_panel_cache_focus();
    test_group9_display_sleep();

    /* App shell (T4): the groups below run with the shell and a fake clock. */
    test_group12_shell_boot_and_esc();
    test_group13_esc_layering_emoji();
    test_group14_switcher_keys();
    test_group15_held_keys();
    test_group16_anim_interrupt();
    test_group17_focus_isolation();
    test_group18_keyboard_hotplug();
    test_group19_lock_with_shell();
    test_group20_boot_music();

    /* Music app (T8). */
#ifdef CHATBOX_HAVE_SQLITE
    test_group21_music_columns();
    test_group22_music_play();
    test_group23_music_now_playing();
    test_group24_music_selection_memory();
    test_group25_music_held_keys();
    test_group26_music_volume();
    test_group27_music_library_changes();
    test_group28_music_fallback_covers();
    test_group29_music_snapshots();
    test_group30_live_tiles();
    test_group31_fast_taps();
#else
    fprintf(stderr, "\n(music groups 21-29 skipped: built without SQLite)\n");
#endif
    shell_teardown();
    test_group32_typing_language();   /* through the shell: after the shell groups */
#ifdef CHATBOX_HAVE_SQLITE
    test_group33_chat_snapshot();
    test_group34_photos();
#endif

    fprintf(stderr, "\n=== Results: %d tests, %d failures ===\n",
            g_tests, g_failures);

    lv_deinit();

    return g_failures > 0 ? 1 : 0;
}
