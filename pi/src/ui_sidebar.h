/**
 * ui_sidebar.h — Contact sidebar with avatars
 *
 * The contacts' shelf beside the open chat: one avatar per contact, the
 * selected one tinted, a red dot for unread. A chat is always open (the
 * kiosk boots into the first contact's, ui_sidebar_open); Tab moves to the
 * next contact, and tapping an avatar opens that contact.
 */

#ifndef CHATBOX_UI_SIDEBAR_H
#define CHATBOX_UI_SIDEBAR_H

#include "lvgl.h"
#include "config.h"

/**
 * Create the sidebar inside a parent container.
 *
 * @param parent     Parent object (left panel of the split layout)
 * @param cfg        Parsed config with contact list
 * @param on_select  Called with contact index when tapped
 */
void ui_sidebar_create(lv_obj_t *parent, const config_t *cfg,
                       void (*on_select)(int contact_idx));

/**
 * Open a contact's chat, as Enter on their row does: on_select(contact_idx),
 * and Tab moves between contacts from then on. Every way of opening a chat
 * goes through here (the row, booting into the first contact).
 */
void ui_sidebar_open(int contact_idx);

/**
 * Highlight the selected contact (or -1 for none).
 */
void ui_sidebar_set_selected(int contact_idx);

/**
 * Set or clear the unread-message indicator for a contact.
 */
void ui_sidebar_set_unread(int contact_idx, bool unread);

/**
 * The unread state behind the dots (the switcher's Chat tile reads it too):
 * how many contacts have unread messages, and the contact whose unread
 * message came last (-1 when none).
 */
int ui_sidebar_unread_count(void);
int ui_sidebar_newest_unread(void);

/** A contact's avatar image source as the sidebar shows it (NULL: none). */
const void *ui_sidebar_avatar_src(int contact_idx);
/** A contact's display name ("" for a bad index). */
const char *ui_sidebar_contact_name(int contact_idx);

/**
 * Called after every change of the unread state (a new unread message, or
 * a contact's unread cleared). NULL to stop.
 */
void ui_sidebar_set_unread_listener(void (*cb)(void));

/**
 * Remove all sidebar contact rows from the default keyboard group.
 */
void ui_sidebar_remove_from_group(void);

/**
 * Add all sidebar contact rows to the given keyboard group.
 */
void ui_sidebar_add_to_group(lv_group_t *grp);

#ifdef CHATBOX_TESTING
lv_obj_t *ui_sidebar_get_row(int idx);
lv_obj_t *ui_sidebar_get_dot(int idx);
int ui_sidebar_get_count(void);
#endif

#endif /* CHATBOX_UI_SIDEBAR_H */
