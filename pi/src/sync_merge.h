/**
 * sync_merge.h — Fold a /sync response into the rooms' timelines (msg_cache).
 *
 * Every room's timeline comes from /sync, as in the canonical Matrix clients
 * (matrix-js-sdk, matrix-rust-sdk): the initial sync (timeline limit 50, the
 * filter in matrix_api.c) seeds each room, incremental syncs append. There is
 * no separate /messages fetch for the visible window, so history and sync
 * can't overlap; events are still deduplicated by event_id (msg_cache_append)
 * as a safety net, e.g. a `limited` sync that repeats events.
 *
 * A `limited` incremental sync (more than the limit arrived while we were
 * away) is appended like any other: the kiosk shows the newest messages and
 * never pages back, so the gap before them is not filled.
 *
 * Own events: `mine` is sender == our user ID. `local_echo` is the server's
 * copy of something this device sent: the optimistic bubble already shows
 * it. It is recognised by unsigned.transaction_id (the server only returns
 * it to the sending device), or by the event_id the send returned
 * (sync_merge_note_sent): a token without a device, such as one made with
 * Synapse's admin login API, gets no transaction_id back. Own events that
 * are neither were sent from another device of the account.
 *
 * Only m.room.message events are kept. Pure: no LVGL, no network.
 *
 * Threading: LVGL thread only (msg_cache).
 */

#ifndef CHATBOX_SYNC_MERGE_H
#define CHATBOX_SYNC_MERGE_H

#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    /* Rooms to keep (the contacts). NULL: every joined room. */
    bool (*wants_room)(const char *room_id, void *ud);
    /* Initial sync: the room's timeline was (re)seeded in msg_cache. */
    void (*on_seeded)(const char *room_id, void *ud);
    /* A message event, after it went into msg_cache: every event of an
     * initial sync (initial = true), and on an incremental sync only the
     * ones msg_cache didn't have yet. */
    void (*on_event)(const char *room_id, const cJSON *event, bool initial,
                     bool mine, bool local_echo, void *ud);
    void *ud;
} sync_merge_ops_t;

/* Remember the event_id a send of ours returned, so its echo in a later
 * sync counts as local_echo. Keeps the last SYNC_MERGE_SENT_MAX. */
#define SYNC_MERGE_SENT_MAX 32
void sync_merge_note_sent(const char *event_id);

/* Apply `sync` (not consumed). `initial`: the response to a sync without
 * `since`. `my_user_id` must be known (whoami); empty means nothing is mine. */
void sync_merge_apply(const cJSON *sync, bool initial, const char *my_user_id,
                      const sync_merge_ops_t *ops);

#endif /* CHATBOX_SYNC_MERGE_H */
