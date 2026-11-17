/**
 * photo_inbox.h — Photos that arrive in chat go into the photo folder
 * (docs/photos.md).
 *
 * Chat is the only way photos get onto the kiosk: an m.image event in a
 * contact's room (sent from a parent's phone) is downloaded once and saved
 * in photos_dir, where the Photos app finds it. Receive only.
 *
 *   photo_inbox_parse()   reads an event: m.room.message, msgtype m.image,
 *                         an mxc:// url. Encrypted images (content.file) and
 *                         anything else are not photos.
 *   photo_inbox_offer()   the chat sees an m.image event (every one the sync
 *                         merges, the initial sync included): queue it,
 *                         unless it is done already (dedupe by event_id).
 *   photo_inbox_tick()    from an lv_timer: starts the next download
 *                         (matrix_async_get_media, one at a time), retries
 *                         failed ones with backoff.
 *   photo_inbox_status()  what a chat bubble shows for an event.
 *
 * Saving: the bytes must sniff as JPEG or PNG (photo_decode.h), whatever the
 * event's mimetype says, and be at most PHOTO_INBOX_MAX_BYTES. They are
 * written to a hidden temp file in photos_dir, fsync'd, then renamed to
 * their final name, so the library never lists half a photo. The file's
 * mtime is set to the event's origin_server_ts, so the Photos stream is in
 * the order the photos were sent.
 *
 * Names: "YYYYMMDD-HHMMSS-<16 hex>.jpg|.png" — the send time (UTC) and a
 * 64-bit FNV-1a hash of the event_id. Only digits, letters and '-': nothing
 * from the event (its body, the sender's file name) reaches the file system.
 * The name is a function of the event, so a saved photo is found again
 * after a restart by computing it.
 *
 * Dedupe: every event that is finished with (saved, or refused for good:
 * not a JPEG/PNG, too big, a 4xx from the server other than 401, 403, 408
 * and 429) goes into a small
 * "seen" list kept in photos_dir/.chatbox-seen (one event_id hash per line,
 * the newest PHOTO_INBOX_SEEN_MAX). So a photo deleted in the Photos app is
 * not downloaded again by the next sync or the next boot. Transient
 * failures (network, 5xx, 401, 403, 408, 429) retry for as long as it
 * takes, like a text in the send outbox: after 2, 8, 20, 40 s, then every
 * 60 s. After PHOTO_INBOX_TRIES of them the bubble shows the failure
 * (status FAILED) while the retries go on; a later success saves the photo
 * and the bubble shows it after all.
 *
 * Without a writable photos_dir nothing is saved (logged once per photo);
 * chat still works.
 *
 * Threading: LVGL thread only (the matrix dispatcher's callbacks).
 */

#ifndef CHATBOX_PHOTO_INBOX_H
#define CHATBOX_PHOTO_INBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define PHOTO_INBOX_MAX_BYTES   (25u * 1024u * 1024u)
#define PHOTO_INBOX_QUEUE       64     /* events tracked this boot */
#define PHOTO_INBOX_TRIES        5     /* transient failures before the bubble says so */
#define PHOTO_INBOX_SEEN_MAX  2048     /* seen-list entries kept */
#define PHOTO_INBOX_NAME_MAX    40     /* "YYYYMMDD-HHMMSS-<16 hex>.jpg" + NUL */

typedef struct {
    char    event_id[256];
    char    mxc[256];
    char    mimetype[64];     /* content.info.mimetype, "" if absent */
    int64_t size;             /* content.info.size, -1 if absent */
    int64_t ts;               /* origin_server_ts (ms), 0 if absent */
    int     w, h;             /* content.info.w / h, 0 if absent */
} photo_event_t;

typedef enum {
    PHOTO_ST_UNKNOWN = 0,     /* never offered (e.g. not a photo) */
    PHOTO_ST_LOADING,         /* queued or downloading */
    PHOTO_ST_READY,           /* saved: the path is filled */
    PHOTO_ST_FAILED,          /* PHOTO_INBOX_TRIES transient errors (still retried) */
    PHOTO_ST_REFUSED,         /* not a JPEG/PNG, too big, rejected */
    PHOTO_ST_GONE,            /* saved once, deleted since (or never readable) */
} photo_status_t;

/* Parse `event` (a timeline event). True when it is an m.image this
 * kiosk can fetch: m.room.message, msgtype m.image, content.url mxc://,
 * an event_id. */
bool photo_inbox_parse(const cJSON *event, photo_event_t *out);

/* The file name for an event (see the header): `ext` is "jpg" or "png". */
void photo_inbox_filename(const char *event_id, int64_t ts_ms, const char *ext,
                          char out[PHOTO_INBOX_NAME_MAX]);

/* 64-bit FNV-1a of the event_id (the seen list and the name use it). */
uint64_t photo_inbox_hash(const char *event_id);

/* A finished photo (saved or refused): who to tell. The UI redraws the
 * event's bubbles and rescans the library. */
typedef void (*photo_inbox_cb_t)(const char *event_id, photo_status_t st, void *ud);

/* Start (or restart: tests) with the folder; loads the seen list. Creates
 * the folder if it is missing (mode 2775), which may fail: then nothing
 * is saved. */
void photo_inbox_init(const char *photos_dir, photo_inbox_cb_t cb, void *ud);
void photo_inbox_deinit(void);

/* See the header. Not a photo, or already done: does nothing. */
void photo_inbox_offer(const cJSON *event);

/* `now_ms`: a monotonic millisecond clock (lv_tick_get()). */
void photo_inbox_tick(uint32_t now_ms);

/* The bubble's view of `event_id` (sent at `ts_ms`). READY fills `path`
 * (n bytes) with the saved file. */
photo_status_t photo_inbox_status(const char *event_id, int64_t ts_ms, char *path, size_t n);

/* Save `data` as the photo of (event_id, ts_ms): sniff, size check, temp
 * file, fsync, rename, mtime. Returns READY (path filled), REFUSED (not a
 * JPEG/PNG, too big, empty) or FAILED (the folder isn't writable). Used by
 * the download callback; public for the tests. */
photo_status_t photo_inbox_save(const char *event_id, int64_t ts_ms,
                                const unsigned char *data, size_t len,
                                char *path, size_t n);

/* The seen list (tests). */
bool photo_inbox_seen(const char *event_id);

#ifdef CHATBOX_TESTING
/* Pretend downloads: when set, tick() calls this instead of the matrix
 * dispatcher (same contract as matrix_async_get_media). */
typedef void (*photo_inbox_media_cb_t)(const unsigned char *data, size_t len, long http_code,
                                       int status, void *userdata);
typedef void (*photo_inbox_fetch_fn)(const char *mxc, bool legacy, size_t max_bytes,
                                     photo_inbox_media_cb_t cb, void *ud);
void photo_inbox_test_set_fetch(photo_inbox_fetch_fn fn);
int  photo_inbox_test_pending(void);
#endif

#endif /* CHATBOX_PHOTO_INBOX_H */
