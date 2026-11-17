/**
 * library.h — Read-only access to the music library DB from the UI process
 * (docs/player-spec.md §2, §7.3–§7.7). The writer is chatbox-indexer; the
 * schema and its conventions are in library_schema.h.
 *
 * Connection
 *   - SQLITE_OPEN_READONLY, WAL (the indexer set the mode). Same user as the
 *     indexer (D6): a WAL reader writes the -shm file. Never immutable=1.
 *   - busy_timeout LIBRARY_BUSY_TIMEOUT_MS (50 ms). library_poll() and every
 *     load run ONE statement, so a call waits at most ~50 ms even while
 *     another connection holds the DB exclusively; a (re)open runs two
 *     (user_version + status). In WAL mode a normal writer never blocks the
 *     reader at all. Every statement is reset right away: no read
 *     transaction is ever held across calls (or frames).
 *   - Missing DB file, or a file the indexer hasn't initialised yet →
 *     LIBRARY_MISSING, logged once. A DB that appears later is opened by
 *     the next poll. A corrupt, unreadable or wrong-schema DB →
 *     LIBRARY_ERROR, logged once per transition; each poll retries. A load
 *     that hits a corrupt page also goes to ERROR, and the reader stays
 *     there (no READY/ERROR flapping) until meta.generation or the file
 *     changes. A DB file replaced on disk (new inode) is reopened.
 *   - The indexer keeps -wal/-shm on close (library_schema.h): some SQLite
 *     builds (Apple's) can't create them for a read-only connection.
 *
 * Polling (§7.7)
 *   library_poll() is cheap (a stat + one prepared statement) and meant for
 *   an lv_timer every 2 s (the caller owns the timer). It returns true when
 *   the UI should reload what it shows: the state changed (e.g. MISSING →
 *   READY), meta.generation changed, the DB was reopened, or an earlier load
 *   failed transiently (busy) and should be retried. It returns false while
 *   the DB is busy, so a change is reported exactly once, when it can be
 *   read.
 *
 * Memory
 *   No allocation after library_open(). Lists are caller-owned structs with
 *   fixed capacity (static or long-lived in the UI; a few hundred KB in
 *   total), filled on navigation or reload, never per frame. Past a cap the
 *   first `cap` rows (in display order) are kept, `total` still holds the
 *   real row count, and one line is logged per list per generation. Strings
 *   are UTF-8, copied with a bound that never splits a code point.
 *
 * Ordering (§2; the UI shows lists in exactly this order)
 *   - all albums (the Albums column, §7.2): title, ASCII case-insensitive,
 *     ignoring a leading "The " (so "The Zoo" sorts as "Zoo"; titles
 *     starting with non-ASCII letters sort after 'z'), then year ascending
 *     with an unknown year LAST, then id.
 *   - artists: sort_name, then name (sort_name case-folds ASCII only, so
 *     names starting with non-ASCII letters sort after 'z'). Not shown by
 *     the v1 UI (artist grouping is deferred, §12); kept for tests and v2.
 *   - albums of an artist: year ascending (an unknown year sorts LAST),
 *     then title (ASCII case-insensitive), then id.
 *   - tracks of an album: disc_no (NULL = 1), then track_no with NULL
 *     (unknown) LAST, then path (= file name within the album), then id.
 *   - all tracks of an artist: albums in album order, each album's tracks
 *     in track order. (Unused by the v1 UI.)
 *
 * Ids and "remember selection" (§7.4)
 *   Ids are the DB rowids. What the indexer keeps stable across passes
 *   (and restarts):
 *   - track.id: stable while the file keeps its path, even when its tags or
 *     contents change (updated in place). A rename or move → a new id.
 *   - album.id: stable while its directory (album.dir) exists with at least
 *     one track, even when tags change. Renaming the directory → a new id.
 *     Under _inbox/ the identity includes the album artist and title tags,
 *     so a retag there → a new id.
 *   - artist.id: stable while an album still has that album artist name. A
 *     renamed artist, or one whose albums all vanished in one pass and came
 *     back later, gets a new id.
 *   - art ids: stable while the image bytes are unchanged (keyed by SHA-1).
 *   The tables have no AUTOINCREMENT, so after the highest id is deleted a
 *   later insert may reuse it. An id is therefore "the same item" in
 *   practice but not guaranteed: at worst a remembered selection lands on
 *   the newcomer. The player's queue holds paths as well (§7.7), so
 *   playback never depends on it.
 *   After a reload, re-find a selection with library_*_find(list, id,
 *   old_index): the index of `id`, else old_index clamped to the list, as
 *   §7.4 asks.
 *
 * Threading: not thread-safe — call only from the LVGL thread.
 */

#ifndef CHATBOX_LIBRARY_H
#define CHATBOX_LIBRARY_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIBRARY_BUSY_TIMEOUT_MS   50

#define LIBRARY_NAME_LEN          256   /* bytes incl. NUL (UTF-8)          */
#define LIBRARY_MAX_ARTISTS       512
#define LIBRARY_MAX_ALBUMS        128   /* per artist                        */
#define LIBRARY_MAX_ALL_ALBUMS    512   /* the whole library (Albums column) */
#define LIBRARY_MAX_TRACKS        256   /* per album                         */
#define LIBRARY_MAX_ARTIST_TRACKS 1024  /* all tracks of one artist (queue)  */

/* Return codes of the lookups and loads. */
#define LIBRARY_OK         0
#define LIBRARY_NOT_FOUND  1
#define LIBRARY_ERR       -1   /* no library, busy, or DB error */

typedef enum {
    LIBRARY_MISSING = 0,    /* no DB file yet, or not initialised yet   */
    LIBRARY_EMPTY,          /* DB open, no tracks                       */
    LIBRARY_READY,          /* DB open, has tracks                      */
    LIBRARY_ERROR,          /* corrupt / unreadable / wrong schema      */
} library_state_t;

typedef struct {
    int64_t id;
    int64_t art_id;         /* 0 = no art: draw the fallback cover (§7.6) */
    int     album_count;
    char    name[LIBRARY_NAME_LEN];
} library_artist_t;

typedef struct {
    int64_t id;
    int64_t artist_id;
    int64_t art_id;         /* 0 = no art */
    int     year;           /* 0 = unknown */
    int     track_count;
    char    title[LIBRARY_NAME_LEN];
    char    artist[LIBRARY_NAME_LEN];   /* the album artist's name */
} library_album_t;

typedef struct {
    int64_t id;
    int64_t album_id;
    int     disc_no;        /* >= 1 */
    int     track_no;       /* 0 = unknown */
    int64_t duration_ms;    /* -1 = unknown */
    char    title[LIBRARY_NAME_LEN];
} library_track_t;

/* Fixed-capacity lists. `count` rows are valid; `total` is the number of
 * rows in the DB (total > count means the list was capped). */
typedef struct {
    int count, total;
    library_artist_t items[LIBRARY_MAX_ARTISTS];
} library_artists_t;

typedef struct {
    int count, total;
    library_album_t items[LIBRARY_MAX_ALBUMS];
} library_albums_t;

typedef struct {
    int count, total;
    library_album_t items[LIBRARY_MAX_ALL_ALBUMS];
} library_all_albums_t;

typedef struct {
    int count, total;
    library_track_t items[LIBRARY_MAX_TRACKS];
} library_tracks_t;

typedef struct {
    int count, total;
    library_track_t items[LIBRARY_MAX_ARTIST_TRACKS];
} library_artist_tracks_t;

/* Everything the player and Now Playing need about one track. */
typedef struct {
    int64_t id;
    int64_t album_id;
    int64_t artist_id;
    int64_t art_id;         /* the album's art; 0 = none */
    int64_t artist_art_id;  /* the album artist's art; 0 = none */
    int     disc_no;
    int     track_no;       /* 0 = unknown */
    int64_t duration_ms;    /* -1 = unknown */
    char    path[PATH_MAX]; /* absolute */
    char    title[LIBRARY_NAME_LEN];
    char    album_title[LIBRARY_NAME_LEN];
    char    artist_name[LIBRARY_NAME_LEN];
} library_track_info_t;

typedef struct library library_t;

/**
 * Create the reader and try to open the DB once. Succeeds even when the DB
 * is missing or broken (see library_state()); the first library_poll()
 * reports only changes after this call.
 * @param art_dir  music_art_dir; NULL → every art path is "none".
 * @return NULL only on out-of-memory or NULL db_path.
 */
library_t *library_open(const char *db_path, const char *art_dir);

void library_close(library_t *lib);

/** Cheap generation check (see "Polling" above). True → reload the lists. */
bool library_poll(library_t *lib);

library_state_t library_state(const library_t *lib);

/** meta.generation as last read, or -1 when there is no library. */
int64_t library_generation(const library_t *lib);

/*
 * Loads. Each fills `out` (count/total 0 on error) and returns LIBRARY_OK,
 * or LIBRARY_ERR when there is no library, the DB is busy or broken. After a
 * transient failure the next library_poll() returns true, so the UI retries.
 * An unknown parent id is not an error: the list is just empty.
 */
int library_load_artists(library_t *lib, library_artists_t *out);
int library_load_albums(library_t *lib, int64_t artist_id, library_albums_t *out);
int library_load_all_albums(library_t *lib, library_all_albums_t *out);
int library_load_tracks(library_t *lib, int64_t album_id, library_tracks_t *out);
int library_load_artist_tracks(library_t *lib, int64_t artist_id,
                               library_artist_tracks_t *out);

/** Look up one track by id. LIBRARY_OK, LIBRARY_NOT_FOUND or LIBRARY_ERR. */
int library_track_get(library_t *lib, int64_t track_id, library_track_info_t *out);

/**
 * Thumbnail path <art_dir>/<art_id>_<size>.png for size 96, 256 or 512.
 * @return false for "no art" (art_id <= 0, no art_dir, unsupported size,
 *         or a path that doesn't fit): the UI draws the fallback cover.
 * The indexer publishes thumbnails before the rows that reference them, but
 * deletes orphans right after a pass, so a list not yet reloaded (≤ one poll
 * interval) can name a file that is gone: if loading it fails, draw the
 * fallback too.
 */
bool library_art_path(const library_t *lib, int64_t art_id, int size,
                      char *out, size_t out_size);

/*
 * Re-find a selection after a reload (§7.4): the index of `id` in the list,
 * else `fallback_index` clamped to [0, count-1]; -1 for an empty list.
 */
int library_artists_find(const library_artists_t *l, int64_t id, int fallback_index);
int library_albums_find(const library_albums_t *l, int64_t id, int fallback_index);
int library_all_albums_find(const library_all_albums_t *l, int64_t id, int fallback_index);
int library_tracks_find(const library_tracks_t *l, int64_t id, int fallback_index);
int library_artist_tracks_find(const library_artist_tracks_t *l, int64_t id,
                               int fallback_index);

/**
 * Copy UTF-8 `src` into `dst` (size n > 0), NUL-terminated. When it doesn't
 * fit, it is cut at a code-point boundary. NULL src → "".
 * @return true if the copy was truncated.
 */
bool library_utf8_copy(char *dst, size_t n, const char *src);

#ifdef CHATBOX_TESTING
struct sqlite3;
/** The reader's connection (NULL when closed). Tests only. */
struct sqlite3 *library_test_db(library_t *lib);
#endif

#endif /* CHATBOX_LIBRARY_H */
