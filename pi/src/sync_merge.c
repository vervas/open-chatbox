/**
 * sync_merge.c — Fold a /sync response into msg_cache (see sync_merge.h).
 */

#include "sync_merge.h"
#include "msg_cache.h"

#include <stdio.h>
#include <string.h>

static bool is_message(const cJSON *event)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(event, "type");
    return cJSON_IsString(type) && strcmp(type->valuestring, "m.room.message") == 0;
}

/* Event IDs our sends returned, oldest overwritten first. */
static char     g_sent[SYNC_MERGE_SENT_MAX][256];
static unsigned g_sent_next;

void sync_merge_note_sent(const char *event_id)
{
    if (!event_id || !event_id[0] || strlen(event_id) >= sizeof(g_sent[0])) return;
    strcpy(g_sent[g_sent_next++ % SYNC_MERGE_SENT_MAX], event_id);
}

/* True (and forgotten) if a send of ours returned this event_id. */
static bool take_sent(const cJSON *event)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(event, "event_id");
    if (!cJSON_IsString(id)) return false;
    for (int i = 0; i < SYNC_MERGE_SENT_MAX; i++) {
        if (g_sent[i][0] && strcmp(g_sent[i], id->valuestring) == 0) {
            g_sent[i][0] = '\0';
            return true;
        }
    }
    return false;
}

static void classify(const cJSON *event, const char *me, bool *mine, bool *echo)
{
    const cJSON *sender = cJSON_GetObjectItemCaseSensitive(event, "sender");
    *mine = me && me[0] && cJSON_IsString(sender) && strcmp(sender->valuestring, me) == 0;
    const cJSON *unsig = cJSON_GetObjectItemCaseSensitive(event, "unsigned");
    const cJSON *txn = cJSON_GetObjectItemCaseSensitive(unsig, "transaction_id");
    *echo = *mine && (take_sent(event) || cJSON_IsString(txn));
}

void sync_merge_apply(const cJSON *sync, bool initial, const char *my_user_id,
                      const sync_merge_ops_t *ops)
{
    const cJSON *rooms = cJSON_GetObjectItemCaseSensitive(sync, "rooms");
    const cJSON *join = cJSON_GetObjectItemCaseSensitive(rooms, "join");
    if (!cJSON_IsObject(join) || !ops) return;

    const cJSON *room = NULL;
    cJSON_ArrayForEach(room, join) {
        const char *room_id = room->string;
        if (!room_id || (ops->wants_room && !ops->wants_room(room_id, ops->ud))) continue;
        const cJSON *timeline = cJSON_GetObjectItemCaseSensitive(room, "timeline");
        const cJSON *events = cJSON_GetObjectItemCaseSensitive(timeline, "events");

        if (initial) {
            /* The room's timeline is exactly the sync's window. */
            cJSON *arr = cJSON_CreateArray();
            if (!arr) continue;
            const cJSON *ev = NULL;
            cJSON_ArrayForEach(ev, events) {
                if (is_message(ev)) cJSON_AddItemToArray(arr, cJSON_Duplicate(ev, 1));
            }
            msg_cache_set(room_id, arr);
            fprintf(stderr, "chat: %s seeded, %d events\n", room_id, cJSON_GetArraySize(arr));
            if (ops->on_seeded) ops->on_seeded(room_id, ops->ud);
            cJSON_ArrayForEach(ev, events) {
                if (!is_message(ev)) continue;
                bool mine, echo;
                classify(ev, my_user_id, &mine, &echo);
                if (ops->on_event) ops->on_event(room_id, ev, true, mine, echo, ops->ud);
            }
            continue;
        }

        int fresh = 0, dup = 0;
        const cJSON *ev = NULL;
        cJSON_ArrayForEach(ev, events) {
            if (!is_message(ev)) continue;
            if (!msg_cache_append(room_id, ev)) { dup++; continue; }
            fresh++;
            bool mine, echo;
            classify(ev, my_user_id, &mine, &echo);
            if (ops->on_event) ops->on_event(room_id, ev, false, mine, echo, ops->ud);
        }
        if (fresh || dup) fprintf(stderr, "chat: %s +%d new, %d dup\n", room_id, fresh, dup);
    }
}
