/**
 * display_sleep.h — Display sleep / lock screen state machine
 *
 * Three states:
 *   ACTIVE  → (1 min idle)  → LOCKED  → (5 min)  → OFF
 *   Any user input returns to ACTIVE from either LOCKED or OFF.
 *   Incoming messages while LOCKED reset the 5-minute countdown.
 *
 * A lock override (display_sleep_set_lock_override) stretches the first
 * step while something is on screen that is meant to be watched without
 * touching the keys: the photo viewer (docs/photos.md).
 */

#ifndef CHATBOX_DISPLAY_SLEEP_H
#define CHATBOX_DISPLAY_SLEEP_H

#include <stdbool.h>

/**
 * Initialise the display sleep module.
 * Creates the dark overlay on lv_layer_top() and starts the
 * periodic inactivity-check timer.  Call once after LVGL and the
 * display backend are fully set up.
 *
 * @param lock_secs  Idle seconds before lock screen (0 = default 60)
 * @param off_secs   Seconds in lock before display off (0 = default 300)
 */
void display_sleep_init(int lock_secs, int off_secs);

/**
 * While `active()` returns true, the idle time before the lock screen is
 * `lock_secs` instead of the normal one; as soon as it returns false the
 * normal timeout applies again (to the idle time so far). The predicate is
 * asked on every check (1 s), so nothing has to report entering or leaving
 * the view. lock_secs <= 0 or active == NULL removes the override. The
 * lock → off step is unchanged.
 */
void display_sleep_set_lock_override(bool (*active)(void), int lock_secs);

/**
 * Notify that a non-self message arrived in any room.
 * If the display is currently LOCKED, resets the 5-minute countdown
 * to display-off and shows a "New message!" indicator.
 */
void display_sleep_notify_message(void);

/**
 * Returns true when the display is in LOCKED or OFF state.
 */
bool display_sleep_is_locked(void);

/**
 * Tear down the overlay and timer.  Call during shutdown.
 */
void display_sleep_deinit(void);

#ifdef CHATBOX_TESTING
#include "lvgl.h"
/**
 * Manually run the inactivity-check timer callback.
 */
void display_sleep_force_check(void);

/**
 * Return the current state: 0 = ACTIVE, 1 = LOCKED, 2 = OFF.
 */
int display_sleep_get_state(void);

/**
 * Return the overlay object (for visibility checks).
 */
lv_obj_t *display_sleep_get_overlay(void);

/**
 * Return the "New message!" label object.
 */
lv_obj_t *display_sleep_get_msg_label(void);
#endif

#endif /* CHATBOX_DISPLAY_SLEEP_H */
