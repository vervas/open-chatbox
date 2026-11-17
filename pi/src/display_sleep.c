/**
 * display_sleep.c — Display sleep / lock screen state machine
 *
 * States:
 *   ACTIVE  --( 1 min idle )-->  LOCKED  --( 5 min )-->  OFF
 *
 * Any user input (keyboard / touch) returns to ACTIVE.
 *
 * The 1 min step is longer while the lock override's predicate holds (the
 * photo viewer: sleep_photo_viewer_mins, 20 min by default).
 * Incoming messages while LOCKED reset the 5-minute countdown and
 * show a "New message!" label on the dark overlay.
 *
 * The first keypress / tap that wakes the display is swallowed:
 * while LOCKED or OFF the overlay is focusable and in the group the
 * keyboard indev is using at lock time (chat's, music's or the
 * switcher's), so it absorbs the input event. On wake, focus goes back
 * to the object that had it at lock time. The shell's keyboard hook also
 * swallows every key press while locked (shell.c), so held keys can't
 * act after the wake either.
 */

#include "display_sleep.h"
#include "lvgl.h"
#include "matrix_api.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef CHATBOX_USE_FB
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#endif

/* ---- Timing constants -------------------------------------------------- */

#define DEFAULT_LOCK_SECS  60              /* 1 minute  → lock screen    */
#define DEFAULT_OFF_SECS   300             /* 5 minutes → display off    */
#define CHECK_MS           1000u           /* poll every 1 s              */

/* ---- State -------------------------------------------------------------- */

typedef enum {
    SLEEP_ACTIVE,
    SLEEP_LOCKED,
    SLEEP_OFF
} sleep_state_t;

static sleep_state_t  g_state          = SLEEP_ACTIVE;
static lv_obj_t      *g_overlay        = NULL;
static lv_obj_t      *g_msg_label      = NULL;
static lv_timer_t    *g_check_timer    = NULL;
static uint32_t       g_lock_start_ms  = 0;   /* tick when we entered LOCKED */
static uint32_t       g_lock_extend_ms = 0;   /* extra ms from message arrivals */
static uint32_t       g_idle_lock_ms   = DEFAULT_LOCK_SECS * 1000u;
static uint32_t       g_lock_off_ms    = DEFAULT_OFF_SECS  * 1000u;
static bool         (*g_override_active)(void) = NULL;
static uint32_t       g_override_ms    = 0;

/* ---- Display power control via FBIOBLANK ------------------------------- */

#ifdef CHATBOX_USE_FB
static void display_power(bool on)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "display_sleep: cannot open /dev/fb0\n");
        return;
    }
    int arg = on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
    int ret = ioctl(fd, FBIOBLANK, arg);
    fprintf(stderr, "display_sleep: FBIOBLANK %s → %s\n",
            on ? "UNBLANK" : "POWERDOWN", ret == 0 ? "ok" : "failed");
    close(fd);
}
#else
static void display_power(bool on)
{
    (void)on;
    fprintf(stderr, "display_sleep: display_power(%s) [SDL2 no-op]\n",
            on ? "on" : "off");
}
#endif

/* ---- Overlay helpers --------------------------------------------------- */

/* The group the overlay joined at lock time, and the object that had focus
 * there, restored on wake. g_prev_focus is cleared if that object is
 * deleted while locked. */
static lv_group_t *g_overlay_group = NULL;
static lv_obj_t   *g_prev_focus    = NULL;

static void prev_focus_deleted_cb(lv_event_t *e)
{
    (void)e;
    g_prev_focus = NULL;
}

static void forget_prev_focus(void)
{
    if (g_prev_focus)
        lv_obj_remove_event_cb(g_prev_focus, prev_focus_deleted_cb);
    g_prev_focus = NULL;
}

/* The group the keyboard is using *now*: chat's, music's or the switcher's
 * (the shell points the keypad indev at the visible one). Falls back to the
 * default group when no keypad has a group. */
static lv_group_t *keyboard_group(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_group(i))
            return lv_indev_get_group(i);
    }
    return lv_group_get_default();
}

static void overlay_show(void)
{
    if (!g_overlay) return;
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Make focusable so the overlay absorbs the first keypress on wake */
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Already in a group (OFF → LOCKED on a message): just keep focus. */
    if (g_overlay_group) {
        lv_group_focus_obj(g_overlay);
        return;
    }

    lv_group_t *grp = keyboard_group();
    if (grp) {
        forget_prev_focus();
        g_prev_focus = lv_group_get_focused(grp);
        if (g_prev_focus)
            lv_obj_add_event_cb(g_prev_focus, prev_focus_deleted_cb,
                                LV_EVENT_DELETE, NULL);
        g_overlay_group = grp;
        lv_group_add_obj(grp, g_overlay);
        lv_group_focus_obj(g_overlay);
    }
}

static void overlay_hide(void)
{
    if (!g_overlay) return;

    /* Give focus back to what had it at lock time, and leave the group,
     * *before* hiding: hiding or removing a focused object makes LVGL
     * focus the next one in the group, which in chat is a sidebar row and
     * (via row_focus_cb) used to switch contact on every wake. */
    if (g_overlay_group) {
        if (g_prev_focus && lv_obj_get_group(g_prev_focus) == g_overlay_group)
            lv_group_focus_obj(g_prev_focus);
        forget_prev_focus();
        lv_group_remove_obj(g_overlay);
        g_overlay_group = NULL;
    }

    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Hide the message indicator for next time */
    if (g_msg_label)
        lv_obj_add_flag(g_msg_label, LV_OBJ_FLAG_HIDDEN);
}

/* ---- State transitions ------------------------------------------------- */

static void enter_locked(void)
{
    g_state          = SLEEP_LOCKED;
    g_lock_start_ms  = lv_tick_get();
    g_lock_extend_ms = 0;
    overlay_show();
    fprintf(stderr, "display_sleep: → LOCKED\n");
}

static void enter_off(void)
{
    g_state = SLEEP_OFF;

    /* Pause display rendering so framebuffer writes don't wake DPMS */
    lv_timer_t *refr = lv_display_get_refr_timer(NULL);
    if (refr) lv_timer_pause(refr);

    display_power(false);
    fprintf(stderr, "display_sleep: → OFF\n");
}

static void enter_active(void)
{
    bool was_asleep = (g_state == SLEEP_LOCKED || g_state == SLEEP_OFF);

    if (g_state == SLEEP_OFF) {
        display_power(true);

        /* Resume display rendering */
        lv_timer_t *refr = lv_display_get_refr_timer(NULL);
        if (refr) lv_timer_resume(refr);
    }
    overlay_hide();
    g_state = SLEEP_ACTIVE;
    fprintf(stderr, "display_sleep: → ACTIVE\n");

    /* Wake counts as user interaction — fast-track any backed-off outbox
     * sends so they retry immediately instead of waiting out their backoff.
     * (Sends already retry forever on their own; this just skips the wait.) */
    if (was_asleep) {
        matrix_async_outbox_resume();
    }
}

/* ---- Timer callback (runs every CHECK_MS) ------------------------------ */

static void sleep_check_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t idle = lv_display_get_inactive_time(NULL);

    switch (g_state) {
    case SLEEP_ACTIVE: {
        uint32_t lock_ms = g_idle_lock_ms;
        if (g_override_active && g_override_ms && g_override_active())
            lock_ms = g_override_ms;
        if (idle >= lock_ms)
            enter_locked();
        break;
    }

    case SLEEP_LOCKED:
        if (idle < g_idle_lock_ms) {
            enter_active();
        } else {
            uint32_t in_lock = lv_tick_elaps(g_lock_start_ms);
            if (in_lock >= g_lock_off_ms + g_lock_extend_ms)
                enter_off();
        }
        break;

    case SLEEP_OFF:
        if (idle < g_idle_lock_ms)
            enter_active();
        break;
    }
}

/* ---- Public API -------------------------------------------------------- */

void display_sleep_init(int lock_secs, int off_secs)
{
    if (lock_secs > 0) g_idle_lock_ms = (uint32_t)lock_secs * 1000u;
    if (off_secs  > 0) g_lock_off_ms  = (uint32_t)off_secs  * 1000u;

    /* Full-screen dark overlay on the top layer */
    g_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_90, 0);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);  /* starts hidden */

    /* "New message!" label — hidden until a message arrives while locked */
    g_msg_label = lv_label_create(g_overlay);
    lv_label_set_text(g_msg_label, "New message!");
    lv_obj_set_style_text_color(g_msg_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_msg_label, &lv_font_montserrat_36, 0);
    lv_obj_center(g_msg_label);
    lv_obj_add_flag(g_msg_label, LV_OBJ_FLAG_HIDDEN);

    /* Periodic inactivity check */
    g_check_timer = lv_timer_create(sleep_check_cb, CHECK_MS, NULL);

    g_state = SLEEP_ACTIVE;
    fprintf(stderr, "display_sleep: initialised\n");
}

void display_sleep_set_lock_override(bool (*active)(void), int lock_secs)
{
    if (!active || lock_secs <= 0) {
        g_override_active = NULL;
        g_override_ms = 0;
        return;
    }
    g_override_active = active;
    g_override_ms = (uint32_t)lock_secs * 1000u;
}

void display_sleep_notify_message(void)
{
    if (g_state == SLEEP_OFF) {
        /* Wake display back to lock screen */
        display_power(true);
        lv_timer_t *refr = lv_display_get_refr_timer(NULL);
        if (refr) lv_timer_resume(refr);

        g_state          = SLEEP_LOCKED;
        g_lock_start_ms  = lv_tick_get();
        g_lock_extend_ms = 0;
        overlay_show();

        if (g_msg_label)
            lv_obj_clear_flag(g_msg_label, LV_OBJ_FLAG_HIDDEN);

        fprintf(stderr, "display_sleep: new message → LOCKED (was OFF)\n");
    } else if (g_state == SLEEP_LOCKED) {
        /* Reset the 5-minute countdown from now */
        g_lock_start_ms  = lv_tick_get();
        g_lock_extend_ms = 0;

        /* Show notification on the lock screen */
        if (g_msg_label)
            lv_obj_clear_flag(g_msg_label, LV_OBJ_FLAG_HIDDEN);
    }
}

bool display_sleep_is_locked(void)
{
    return g_state == SLEEP_LOCKED || g_state == SLEEP_OFF;
}

void display_sleep_deinit(void)
{
    if (g_state == SLEEP_OFF) {
        display_power(true);
        lv_timer_t *refr = lv_display_get_refr_timer(NULL);
        if (refr) lv_timer_resume(refr);
    }

    if (g_check_timer) {
        lv_timer_delete(g_check_timer);
        g_check_timer = NULL;
    }
    if (g_overlay) {
        /* Remove from its group before deleting */
        forget_prev_focus();
        lv_group_remove_obj(g_overlay);
        g_overlay_group = NULL;
        lv_obj_delete(g_overlay);
        g_overlay   = NULL;
        g_msg_label = NULL;
    }
    g_state = SLEEP_ACTIVE;
    fprintf(stderr, "display_sleep: deinitialised\n");
}

#ifdef CHATBOX_TESTING
void display_sleep_force_check(void)
{
    sleep_check_cb(NULL);
}

int display_sleep_get_state(void)
{
    return (int)g_state;
}

lv_obj_t *display_sleep_get_overlay(void)
{
    return g_overlay;
}

lv_obj_t *display_sleep_get_msg_label(void)
{
    return g_msg_label;
}
#endif
