/**
 * matrix_api.h — Matrix client-server API client
 *
 * Talks to a Matrix homeserver (e.g. Dendrite) over HTTP using libcurl.
 * Parses responses with cJSON.
 */

#ifndef CHATBOX_MATRIX_API_H
#define CHATBOX_MATRIX_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * Initialize the Matrix API client.
 * Must be called once before any other matrix_* functions.
 *
 * @param homeserver_url  Base URL, e.g. "https://matrix.example.org"
 * @param access_token    Bearer token for authentication
 */
void matrix_init(const char *homeserver_url, const char *access_token);

/**
 * Fetch message history for a room (backwards from most recent).
 *
 * @param room_id  Matrix room ID, e.g. "!abc:localhost"
 * @param limit    Max number of messages to return
 * @return cJSON array of timeline events, or NULL on error.
 *         Each event has: event_id, sender, content.body, content.msgtype,
 *         origin_server_ts. Caller must free with cJSON_Delete().
 */
cJSON *matrix_get_messages(const char *room_id, int limit);

/**
 * Send a text message to a room.
 *
 * @param room_id  Matrix room ID
 * @param text     Message text
 * @return 0 on success, -1 on error
 */
int matrix_send_message(const char *room_id, const char *text);

/**
 * Perform an incremental sync to get new events.
 *
 * @param since  Sync token from a previous sync (NULL or "" for initial sync)
 * @return cJSON object with "next_batch" and "rooms.join.{room_id}.timeline.events"
 *         Caller must free with cJSON_Delete(). NULL on error.
 */
cJSON *matrix_sync(const char *since);

/**
 * Get the authenticated user's ID.
 *
 * @return cJSON object with "user_id" and "device_id".
 *         Caller must free with cJSON_Delete(). NULL on error.
 */
cJSON *matrix_whoami(void);

/**
 * Clean up libcurl resources.
 */
void matrix_cleanup(void);

/* -------------------------------------------------------------------------
 * Async API (Phase B — driven by curl_multi)
 *
 * Every request runs through a small FIFO queue. matrix_async_tick() must
 * be called periodically (e.g. from an lv_timer) so curl_multi_perform
 * gets a chance to drive IO and fire completion callbacks. The callbacks
 * run on whatever thread calls matrix_async_tick — by design that is the
 * LVGL main thread.
 *
 * The async dispatcher serialises requests: only one easy handle is
 * in-flight at a time. Real overlap is rare in this workload so this
 * keeps buffer ownership simple.
 * ---------------------------------------------------------------------- */

/* Status codes passed to async callbacks. */
#define MATRIX_OK              0
#define MATRIX_ERR_TRANSPORT  -1   /* libcurl error (timeout, DNS, etc.) */
#define MATRIX_ERR_HTTP       -2   /* HTTP status != 200 */
#define MATRIX_ERR_PARSE      -3   /* response wasn't valid JSON */
#define MATRIX_ERR_QUEUE      -4   /* request queue full */
#define MATRIX_ERR_CANCELLED  -5   /* request was cancelled */
#define MATRIX_ERR_REJECTED   -6   /* server refused the request permanently
                                    * (HTTP 4xx except 408/429): no retry, the
                                    * outbox drops the entry instead of looping */
#define MATRIX_ERR_TOO_LARGE  -7   /* media download bigger than its limit */

typedef void (*matrix_async_cb_t)(cJSON *result, int status, void *userdata);

/* Initialise the async dispatcher (creates the curl multi handle and
 * empties the queue). Idempotent. Called automatically by matrix_init. */
void matrix_async_init(void);

/* Drive the dispatcher — call from an lv_timer. Non-blocking. */
void matrix_async_tick(void);

/* Async variant of matrix_sync. cb fires on the LVGL thread when the
 * sync response arrives or fails. cJSON ownership transfers to cb on
 * success; cb receives NULL when status != MATRIX_OK. */
void matrix_async_sync(const char *since, matrix_async_cb_t cb, void *userdata);

/* Async variant of matrix_get_messages. Like matrix_get_messages, the
 * cJSON passed to cb is the "chunk" array (newest-first), already
 * detached from the parent object. */
void matrix_async_get_messages(const char *room_id, int limit,
                               matrix_async_cb_t cb, void *userdata);

/* Async variant of matrix_send_message. cb receives the response cJSON
 * (typically just an event_id) on success. */
void matrix_async_send_message(const char *room_id, const char *text,
                               matrix_async_cb_t cb, void *userdata);

/* Async variant of matrix_whoami. */
void matrix_async_whoami(matrix_async_cb_t cb, void *userdata);

/* Media download (an incoming photo, docs/photos.md). Binary, so its
 * callback gets bytes, not cJSON. `data` is only valid during the call
 * (copy what you keep); NULL / 0 unless status is MATRIX_OK. `http_code`
 * is the HTTP status (0: none, e.g. a transport error), so the caller can
 * tell a permanent 4xx from a transient failure. */
typedef void (*matrix_media_cb_t)(const unsigned char *data, size_t len, long http_code,
                                  int status, void *userdata);

/* Split "mxc://<server>/<media id>" into its two parts, checking them
 * against the Matrix grammar (server: letters, digits, '.', '-', ':', '[',
 * ']'; media id: letters, digits, '_', '-'), so neither can smuggle a path
 * or query into the download URL. Returns false on anything else. */
bool matrix_mxc_parse(const char *mxc, char *server, size_t server_n,
                      char *media_id, size_t media_n);

/* Download `mxc` in the media slot, beside the main queue (a photo that
 * takes minutes doesn't hold up /sync or sends). `legacy` false uses the
 * authenticated endpoint (GET /_matrix/client/v1/media/download/…, Matrix
 * 1.11); true the old unauthenticated one (/_matrix/media/v3/download/…),
 * for servers that don't have the new one yet (they answer 404 / 400
 * M_UNRECOGNIZED). Bodies over `max_bytes` are cut off with
 * MATRIX_ERR_TOO_LARGE. A bad mxc fails at once with MATRIX_ERR_REJECTED.
 * One download at a time: while one is in flight another fails at once
 * with MATRIX_ERR_QUEUE (photo_inbox.c queues them and asks again). */
void matrix_async_get_media(const char *mxc, bool legacy, size_t max_bytes,
                            matrix_media_cb_t cb, void *userdata);

/* Cancel any pending or in-flight requests of the given type. Their
 * callbacks fire with MATRIX_ERR_CANCELLED so userdata still gets freed.
 * Pass type == -1 to cancel all. */
void matrix_async_cancel(int type);

/* Fast-track retries: reset the backoff schedule on every outbox entry
 * (attempts back to 0, next-retry cleared) so any backed-off send retries
 * immediately instead of waiting out its remaining backoff. Sends already
 * retry forever on their own; this just skips the wait. Call on display
 * wake-to-ACTIVE; matrix_async_send_message already invokes it itself, so
 * callers don't need to. */
void matrix_async_outbox_resume(void);

/* Testing hook: shift the monotonic clock used for backoff scheduling
 * by `delta_ms`. Production code must never call this. */
void matrix_async__test_advance_clock_ms(int64_t delta_ms);

/* Testing hook: simulate the head-of-outbox send completing with `http_code`,
 * driving the real completion path (200 success, 4xx permanent rejection, or
 * transient backoff). Production code must never call this. */
void matrix_async__test_finish_send_http(long http_code);

#endif /* CHATBOX_MATRIX_API_H */
