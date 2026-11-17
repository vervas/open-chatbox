/**
 * shell.h — App shell: registry, focus groups, key routing, switcher
 * (docs/player-spec.md §5, §6, §8).
 *
 * The shell owns which app is visible, whether the switcher is open, the
 * one key filter (D2) and the keyboard indevs. See shell.c for how keys
 * are routed.
 */

#ifndef CHATBOX_SHELL_H
#define CHATBOX_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "app.h"
#include "key_filter.h"

/* Reset the registry and the key filter. Call once after lv_init(), before
 * any shell_attach_keyboard(). */
void shell_init(void);

/* Register an app. `filters_own_keys` is true when the app's LV_EVENT_KEY
 * handlers run every key through shell_filter_key() (music, and the
 * switcher internally). For an app that takes LVGL keys directly (chat),
 * the shell records each key press itself so the D2 record stays global.
 * Up to SHELL_MAX_APPS apps; the order is the switcher's tile order. */
#define SHELL_MAX_APPS 4
void shell_register_app(const app_t *app, bool filters_own_keys);

/* Create every registered app (hidden) and the switcher, then show the app
 * whose id is `boot_app_id` (falls back to the first app). The shell keeps
 * the group an app leaves as LVGL's default (chat's) as the default. */
void shell_start(const char *boot_app_id);

/* Wrap a keypad indev's read callback with the shell's key hook and point
 * it at the active group. Call for every keyboard, including one attached
 * again after a hotplug. Safe before shell_start(). */
void shell_attach_keyboard(lv_indev_t *indev);

/* Release reset (docs/player-spec.md §8.1): a real key release clears that
 * key's one-shot record, so every separate press acts. By default a hooked
 * keyboard's own release edges are taken as real (libinput, test indevs).
 * LVGL's SDL driver fakes a release right after every press, OS auto-repeats
 * included; mark such a keyboard with on = true (after
 * shell_attach_keyboard) and report SDL's queue with the two calls below. */
void shell_keyboard_use_external_release(lv_indev_t *indev, bool on);

/* For external-release keyboards, in the order the events are queued (an
 * SDL event watch): a press of `key` (LVGL key code) that LVGL will deliver
 * later — every SDL_KEYDOWN it maps, OS repeats included, and each ' ' of
 * an SDL_TEXTINPUT — and the key's real key-up. The shell pairs each
 * queued press with the one LVGL delivers, so the key-up takes effect
 * after the presses queued before it, however late LVGL handles them. */
void shell_key_pushed(uint32_t key);
void shell_key_released(uint32_t key);

/* The group the keyboard indev should use right now: the switcher's while
 * it is open, else the visible app's. NULL before shell_start(). */
lv_group_t *shell_active_group(void);

/* The visible app (under the switcher, if it is open). */
const app_t *shell_visible_app(void);
bool shell_switcher_is_open(void);

/* Show app `id` and close the switcher. No-op for an unknown id. */
void shell_switch_to(const char *id);
void shell_open_switcher(void);
void shell_close_switcher(void);

/* Live tiles (T9): app `id`'s tile content changed. While the switcher is
 * open the shell re-renders that tile (app->tile_render); while it is
 * closed this does nothing — every tile is rendered when it opens. Apps
 * call it from their change callbacks. */
void shell_refresh_tile(const char *id);

/* Peek pills: `pill` (ui_peek_create, in the host app's header) shows app
 * `src_id`'s news (app->peek_render). Rendered at shell_start and on every
 * shell_refresh_tile(src_id), whether the switcher is open or not. */
void shell_attach_peek(lv_obj_t *pill, const char *src_id);

/* D2 filter against the shell's single, global record: records `key` now
 * and returns false when it is one-shot in `ctx` and the same key had an
 * event less than KEY_FILTER_WINDOW_MS ago with no release in between, or
 * is an LVGL repeat of a key still held. */
bool shell_filter_key(uint32_t key, key_ctx_t ctx);

/* Call from an LV_EVENT_KEY handler whose key starts a context change that
 * finishes later (the switcher's zoom): the rest of that press (repeats,
 * RELEASED/CLICKED) is then not delivered anywhere. The shell does this
 * itself for the switches it performs. */
void shell_consume_press(void);

/* Esc that reached an object through LVGL instead of the read hook (e.g.
 * an indev the shell has not wrapped): filter it and route it exactly as
 * the hook would. */
void shell_handle_esc(void);

/* LVGL auto-adds some widget types (buttons, text areas…) to the default
 * group at creation. The default group is chat's, so code that builds
 * widgets for another app or the switcher wraps it in these two calls. */
lv_group_t *shell_suspend_default_group(void);
void        shell_restore_default_group(lv_group_t *saved);

#ifdef CHATBOX_TESTING
/* Delete the switcher and every app root; forget the apps. Deletes the
 * non-default groups; the default (chat's) group is left to the caller. */
void shell_deinit(void);
key_filter_t *shell_get_key_filter(void);
#endif

#endif /* CHATBOX_SHELL_H */
