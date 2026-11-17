/**
 * key_filter.h — One-shot key filter (player spec D2, §5 step 2, §8.1).
 *
 * The shell keeps, per key code, the time of that key's last event
 * (accepted or dropped). A one-shot key event is dropped when the same key
 * had an event less than KEY_FILTER_WINDOW_MS earlier; exactly
 * KEY_FILTER_WINDOW_MS is accepted. Non-one-shot keys are always accepted
 * but their time is still recorded. The record is global: it is never reset
 * when the context (visible app / switcher) changes.
 *
 * A real key release clears that key's record (key_filter_release), so every
 * separate press acts, however fast. Only events that arrive with no release
 * in between — repeats of a key that is still held — meet the window.
 *
 * Consequence: a held one-shot key fires exactly once. Its repeats (LVGL's
 * on the Pi, the OS's on SDL) come without a release, each within 450 ms
 * of the previous (dropped) event, so the window slides.
 *
 * Pure C, no LVGL dependency: key codes are plain uint32_t (same values as
 * LVGL's LV_KEY_* and the patched driver codes, see KF_KEY_* below) and
 * time is the caller's uint32_t millisecond tick (lv_tick_get()). Tick
 * arithmetic is unsigned, so the 2^32 wrap-around is handled.
 *
 * Storage: a fixed table of KEY_FILTER_SLOTS entries inside key_filter_t.
 * Every key is recorded, including keys the §8.1 table doesn't name
 * (letters typed in chat, etc.). When the table is full, the entry with
 * the oldest time is reused. A key with no entry counts as "no previous
 * event", so it is accepted: eviction never drops a fresh key, and it can
 * only lose a record that is itself older than the window unless more than
 * KEY_FILTER_SLOTS distinct keys arrive within 450 ms (not humanly possible).
 *
 * Known limit: a key untouched for a multiple of ~49.7 days (2^32 ms) looks
 * recent to unsigned arithmetic; at worst one press of it is dropped.
 *
 * Threading: not thread-safe — call only from the LVGL thread.
 */

#ifndef CHATBOX_KEY_FILTER_H
#define CHATBOX_KEY_FILTER_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_FILTER_WINDOW_MS 450u
#define KEY_FILTER_SLOTS     32

/* Key codes the §8.1 table names. Values match lv_group.h's LV_KEY_* and
 * pi/patches/lvgl-keyboard-drivers.patch; kept local so this module stays
 * LVGL-free. The shell (T4) should static_assert they still agree. */
#define KF_KEY_ENTER   10u    /* LV_KEY_ENTER */
#define KF_KEY_ESC     27u    /* LV_KEY_ESC */
#define KF_KEY_UP      17u    /* LV_KEY_UP */
#define KF_KEY_DOWN    18u    /* LV_KEY_DOWN */
#define KF_KEY_RIGHT   19u    /* LV_KEY_RIGHT */
#define KF_KEY_LEFT    20u    /* LV_KEY_LEFT */
#define KF_KEY_SPACE   32u    /* ' ' */
#define KF_KEY_PGUP    0x05u  /* patched PageUp */
#define KF_KEY_PGDN    0x06u  /* patched PageDown */
#define KF_KEY_MENU    0x04u  /* patched Win/Menu/Super (emoji toggle) */
#define KF_KEY_DEL     127u   /* LV_KEY_DEL: the Delete key (Photos) */

/* Contexts of the §8.1 one-shot table. */
typedef enum {
    KEY_CTX_MUSIC_BROWSE = 0, /* Albums / Songs */
    KEY_CTX_NOW_PLAYING,
    KEY_CTX_NO_MUSIC,          /* "No music yet" */
    KEY_CTX_SWITCHER,
    KEY_CTX_CHAT,
    KEY_CTX_PHOTOS,            /* grid, viewer, delete confirm */
    KEY_CTX_COUNT
} key_ctx_t;

typedef struct {
    uint32_t key;
    uint32_t last_ms;
    bool     used;
} key_filter_slot_t;

typedef struct {
    key_filter_slot_t slots[KEY_FILTER_SLOTS];
} key_filter_t;

/* Clear every record. */
void key_filter_init(key_filter_t *kf);

/* Record `key` at `now_ms` (always, accepted or not) and decide whether it
 * acts. Returns false only when `one_shot` is true and the same key's
 * previous event was less than KEY_FILTER_WINDOW_MS before `now_ms`. */
bool key_filter_accept(key_filter_t *kf, uint32_t key, uint32_t now_ms,
                       bool one_shot);

/* `key` was really released: forget its record, so its next press is
 * accepted whatever the time since. No-op for a key with no record. The
 * caller decides what counts as a real release (shell.c: the indev's
 * release edge on the Pi, SDL's key-up on the desktop). */
void key_filter_release(key_filter_t *kf, uint32_t key);

/* Record without deciding: the display-locked path (§5 step 1, §8.7), where
 * the lock overlay swallows the key but its time must still count. */
void key_filter_record(key_filter_t *kf, uint32_t key, uint32_t now_ms);

/* The §8.1 one-shot table. Keys it doesn't name, and unknown contexts,
 * are not one-shot. */
bool key_filter_is_one_shot(uint32_t key, key_ctx_t ctx);

#endif /* CHATBOX_KEY_FILTER_H */
