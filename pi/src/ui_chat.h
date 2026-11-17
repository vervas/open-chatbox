/**
 * ui_chat.h — Chat message screen
 *
 * Shows a scrollable message thread with a text input bar at the bottom.
 * Designed to live inside a content panel alongside a sidebar.
 */

#ifndef CHATBOX_UI_CHAT_H
#define CHATBOX_UI_CHAT_H

#include "lvgl.h"
#include "lang.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Create the chat screen inside a content panel.
 *
 * @param parent        Parent container — becomes the per-room root panel.
 *                      All chat widgets are added as children. The panel's
 *                      user_data is set to a per-panel state struct so that
 *                      ui_chat_attach() can re-bind globals later.
 * @param contact_name  Display name shown in the header
 * @param room_id       Matrix room ID for this conversation
 */
void ui_chat_create(lv_obj_t *parent, const char *contact_name,
                    const char *room_id);

/**
 * Re-bind the module-level globals to the widgets owned by `panel`.
 * `panel` must have been created by a prior ui_chat_create() call.
 * Useful for the LRU panel cache: hide the old panel, attach to the
 * cached one, and unhide.
 */
void ui_chat_attach(lv_obj_t *panel);

/**
 * Detach: clear the module-level globals so subsequent ui_chat_*
 * calls become no-ops. Does NOT destroy the panel widgets — used
 * when temporarily switching away.
 */
void ui_chat_detach(void);

/**
 * Free the per-panel state attached to `panel` (does not delete the
 * panel itself — caller is expected to do that or hide it).
 */
void ui_chat_free_panel_state(lv_obj_t *panel);

/**
 * Append a message bubble to the conversation.
 */
void ui_chat_add_message(const char *text, bool is_from_me);

/**
 * Append a message bubble to a specific cached panel (active or not).
 * Used so background events appear in cached but currently-hidden panels.
 */
void ui_chat_add_message_to(lv_obj_t *panel, const char *text, bool is_from_me);

/**
 * Append a photo bubble (an m.image event, docs/photos.md) to the active
 * panel / to `panel`. `ts` is the event's origin_server_ts and w × h its
 * content.info size (0 when unknown): the bubble takes the photo's shape
 * before the picture arrives. The picture comes from photo_inbox (the saved
 * file); until then, or if it never comes, the bubble shows a glyph.
 */
void ui_chat_add_photo(const char *event_id, int64_t ts, int w, int h, bool is_from_me);
void ui_chat_add_photo_to(lv_obj_t *panel, const char *event_id, int64_t ts, int w, int h,
                          bool is_from_me);

/**
 * photo_inbox finished with `event_id` (saved, refused or failed): update
 * every bubble of it, in every panel.
 */
void ui_chat_photo_changed(const char *event_id);

/**
 * Register the callback invoked when the user presses Send. `send_ctx`
 * is an opaque token that ui_chat owns and the callback should pass to
 * the network layer as userdata. The same token is later handed to
 * ui_chat_pending_complete() so ui_chat can locate the optimistic
 * bubble that goes with this send.
 */
void ui_chat_set_send_callback(void (*on_send)(const char *text, void *send_ctx));

/**
 * Report the network-layer outcome of a previously-enqueued send.
 * `send_ctx` is the value ui_chat handed to the callback in
 * ui_chat_set_send_callback. `failed = true` recolours the bubble red
 * permanently (the visual contract is "this did not arrive cleanly");
 * `failed = false` leaves the bubble blue. Idempotent and safe to call
 * with a context whose bubble has since been destroyed.
 */
void ui_chat_pending_complete(void *send_ctx, bool failed);

/**
 * Clean up chat state (nulls pointers, does not destroy parent).
 * Equivalent to ui_chat_detach() — kept for compatibility.
 */
void ui_chat_destroy(void);

/**
 * Return the room_id that was passed to ui_chat_create.
 */
const char *ui_chat_get_room_id(void);

/**
 * Return the room_id stored on a (possibly inactive) panel.
 */
const char *ui_chat_panel_room_id(lv_obj_t *panel);

/**
 * Insert a "New" divider in the message area (idempotent — only one at a time).
 */
void ui_chat_insert_new_divider(void);

/**
 * Remove the "New" divider if present.
 */
void ui_chat_clear_new_divider(void);

/**
 * Close the emoji panel if it is open (same as Esc / Win while it has
 * focus). Returns true if a panel was closed. Used by the shell's Esc
 * layering: chat consumes Esc only while the panel is open.
 */
bool ui_chat_close_emoji_panel(void);

/* What chat shows now (key hints): a conversation, and its emoji panel. */
bool ui_chat_is_open(void);
bool ui_chat_emoji_open(void);

/* The contact's face in the active panel's header (an image source as
 * ui_sidebar_avatar_src returns; NULL: their initial). */
void ui_chat_set_avatar(const void *src);

/**
 * Set the active panel's typing language: turns on that language's
 * ` + vowel accent combo in its textarea (LANG_NONE turns it off). Stored
 * per panel, so it survives detach/attach. The keyboard layout is main.c's
 * job.
 */
void ui_chat_set_language(chat_lang_t lang);

#ifdef CHATBOX_TESTING
#include <stddef.h>
lv_obj_t   *ui_chat_get_textarea(void);
lv_obj_t   *ui_chat_get_send_btn(void);
lv_obj_t   *ui_chat_get_emoji_panel(void);
lv_obj_t   *ui_chat_get_msg_cont(void);
lv_obj_t   *ui_chat_get_new_divider(void);
size_t      ui_chat_get_emoji_page_idx(void);
size_t      ui_chat_get_emoji_page_count(void);
const char *ui_chat_get_emoji_page_title(void);
size_t      ui_chat_get_emoji_page_entry_count(void);
/* Photo bubbles in the active panel: how many, and how many show their
 * picture (after ui_chat_test_flush_photos decoded the waiting ones). */
int         ui_chat_test_photo_bubbles(int *with_picture);
void        ui_chat_test_flush_photos(void);
/* A bubble of `event_id` (any panel) shows its picture; the bytes of all
 * decoded bubble pictures. */
bool        ui_chat_test_photo_shows(const char *event_id);
size_t      ui_chat_test_photo_bytes(void);
#endif

#endif /* CHATBOX_UI_CHAT_H */
