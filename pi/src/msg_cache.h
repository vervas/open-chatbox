/**
 * msg_cache.h — In-memory cache of Matrix message arrays per room.
 *
 * Stores up to MSG_CACHE_MAX_ROOMS rooms; each room holds a cJSON array of
 * timeline events (oldest-first). Cache lifetime = process lifetime.
 *
 * Threading: not thread-safe — call only from the LVGL thread.
 */

#ifndef CHATBOX_MSG_CACHE_H
#define CHATBOX_MSG_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#define MSG_CACHE_MAX_ROOMS 16
/* Events kept per room: the oldest go first past this (a long uptime). */
#define MSG_CACHE_MAX_EVENTS 200

/* Get cached messages for room_id, or NULL if not cached.
 * Returned cJSON is owned by the cache — do NOT free or detach. */
cJSON *msg_cache_get(const char *room_id);

/* Replace the cache entry for room_id with `messages` (oldest-first).
 * Cache takes ownership of the cJSON array. If the room already had
 * an entry, the old one is freed. If `messages` is NULL the entry
 * is removed. Evicts the LRU entry if the table is full. */
void msg_cache_set(const char *room_id, cJSON *messages);

/* Append one event to the cache for room_id. The event is duplicated
 * (so the caller can free its source). If the room is not cached yet,
 * this creates a fresh array containing only that event. An event whose
 * event_id the room already holds is skipped. Returns true when the event
 * was added. Past MSG_CACHE_MAX_EVENTS the oldest event is dropped. */
bool msg_cache_append(const char *room_id, const cJSON *event);

/* Free everything (called from matrix_cleanup). */
void msg_cache_clear_all(void);

#ifdef CHATBOX_TESTING
int  msg_cache_count(void);
#endif

#endif /* CHATBOX_MSG_CACHE_H */
