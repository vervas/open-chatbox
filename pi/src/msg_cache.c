/**
 * msg_cache.c — Per-room message cache implementation.
 *
 * Open-addressed table keyed by room_id, with LRU eviction when full.
 * Memory budget: ~16 rooms × 50 events × ~200 bytes ≈ 160 KB.
 */

#include "msg_cache.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char     room_id[256];
    cJSON   *messages;
    uint64_t last_used;   /* monotonic counter */
} cache_entry_t;

static cache_entry_t g_entries[MSG_CACHE_MAX_ROOMS];
static uint64_t      g_lru_counter = 0;

static cache_entry_t *find_entry(const char *room_id)
{
    if (!room_id || !room_id[0]) return NULL;
    for (int i = 0; i < MSG_CACHE_MAX_ROOMS; i++) {
        if (g_entries[i].messages != NULL &&
            strcmp(g_entries[i].room_id, room_id) == 0) {
            return &g_entries[i];
        }
    }
    return NULL;
}

static cache_entry_t *find_free_or_lru(void)
{
    cache_entry_t *lru = &g_entries[0];
    for (int i = 0; i < MSG_CACHE_MAX_ROOMS; i++) {
        if (g_entries[i].messages == NULL) return &g_entries[i];
        if (g_entries[i].last_used < lru->last_used) lru = &g_entries[i];
    }
    /* Evict LRU */
    if (lru->messages) {
        cJSON_Delete(lru->messages);
        lru->messages = NULL;
        lru->room_id[0] = '\0';
    }
    return lru;
}

cJSON *msg_cache_get(const char *room_id)
{
    cache_entry_t *e = find_entry(room_id);
    if (!e) return NULL;
    e->last_used = ++g_lru_counter;
    return e->messages;
}

void msg_cache_set(const char *room_id, cJSON *messages)
{
    if (!room_id || !room_id[0]) {
        if (messages) cJSON_Delete(messages);
        return;
    }

    cache_entry_t *e = find_entry(room_id);
    if (e) {
        if (e->messages != messages) cJSON_Delete(e->messages);
        e->messages = messages;
        e->last_used = ++g_lru_counter;
        if (!messages) e->room_id[0] = '\0';
        return;
    }

    if (!messages) return;  /* nothing to insert */

    e = find_free_or_lru();
    strncpy(e->room_id, room_id, sizeof(e->room_id) - 1);
    e->room_id[sizeof(e->room_id) - 1] = '\0';
    e->messages = messages;
    e->last_used = ++g_lru_counter;
}

/* Does `arr` hold an event with this event_id? Newest first (a repeat is
 * almost always recent); cJSON keeps an array's last item in child->prev. */
static bool has_event_id(const cJSON *arr, const char *id)
{
    const cJSON *first = arr ? arr->child : NULL;
    if (!first) return false;
    for (const cJSON *ev = first->prev; ; ev = ev->prev) {
        const cJSON *eid = cJSON_GetObjectItemCaseSensitive(ev, "event_id");
        if (cJSON_IsString(eid) && strcmp(eid->valuestring, id) == 0) return true;
        if (ev == first) return false;
    }
}

bool msg_cache_append(const char *room_id, const cJSON *event)
{
    if (!room_id || !room_id[0] || !event) return false;

    cache_entry_t *e = find_entry(room_id);
    const cJSON *eid = cJSON_GetObjectItemCaseSensitive(event, "event_id");
    if (e && cJSON_IsString(eid) && has_event_id(e->messages, eid->valuestring))
        return false;

    cJSON *dup = cJSON_Duplicate(event, 1);
    if (!dup) return false;

    if (e && cJSON_IsArray(e->messages)) {
        cJSON_AddItemToArray(e->messages, dup);
        while (cJSON_GetArraySize(e->messages) > MSG_CACHE_MAX_EVENTS)
            cJSON_DeleteItemFromArray(e->messages, 0);
        e->last_used = ++g_lru_counter;
        return true;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        cJSON_Delete(dup);
        return false;
    }
    cJSON_AddItemToArray(arr, dup);
    msg_cache_set(room_id, arr);
    return true;
}

void msg_cache_clear_all(void)
{
    for (int i = 0; i < MSG_CACHE_MAX_ROOMS; i++) {
        if (g_entries[i].messages) {
            cJSON_Delete(g_entries[i].messages);
            g_entries[i].messages = NULL;
        }
        g_entries[i].room_id[0] = '\0';
        g_entries[i].last_used = 0;
    }
    g_lru_counter = 0;
}

#ifdef CHATBOX_TESTING
int msg_cache_count(void)
{
    int n = 0;
    for (int i = 0; i < MSG_CACHE_MAX_ROOMS; i++) {
        if (g_entries[i].messages) n++;
    }
    return n;
}
#endif
