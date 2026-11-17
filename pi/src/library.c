/**
 * library.c — Read-only music library reader for the UI. See library.h.
 */

#include "library.h"
#include "library_schema.h"
#include "text_nfc.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Prepared statements, created on (re)open and finalised on close. */
enum {
    ST_STATUS,          /* generation + "has tracks" in one statement */
    ST_ARTISTS,
    ST_ALBUMS,
    ST_ALL_ALBUMS,
    ST_TRACKS,
    ST_ARTIST_TRACKS,
    ST_TRACK_GET,
    ST_COUNT
};

/* Track columns shared by ST_TRACKS and ST_ARTIST_TRACKS (see fill_track). */
#define TRACK_COLS "t.id, t.album_id, COALESCE(t.disc_no, 1), t.track_no, t.duration_ms, t.title"
#define TRACK_ORDER "COALESCE(t.disc_no, 1), t.track_no IS NULL, t.track_no, t.path, t.id"
#define ALBUM_ORDER "al.year IS NULL, al.year, al.title COLLATE NOCASE, al.title, al.id"
/* Every album (the Albums column, §7.2): by title, ASCII case-insensitive,
 * a leading "The " ignored; then year with an unknown year last; then id. */
#define ALL_ALBUM_ORDER                                                         \
    "CASE WHEN lower(substr(COALESCE(al.title, ''), 1, 4)) = 'the '"            \
    " THEN ltrim(substr(al.title, 5)) ELSE COALESCE(al.title, '') END"          \
    " COLLATE NOCASE, COALESCE(al.year, 0) <= 0, al.year, al.id"
/* Album columns shared by ST_ALBUMS and ST_ALL_ALBUMS (see fill_album). */
#define ALBUM_COLS                                                              \
    "al.id, al.artist_id, al.art_id, al.year,"                                  \
    " (SELECT count(*) FROM track t WHERE t.album_id = al.id), al.title, ar.name" \
    " FROM album al LEFT JOIN artist ar ON ar.id = al.artist_id"

static const char *const k_sql[ST_COUNT] = {
    [ST_STATUS] =
        "SELECT (SELECT CAST(value AS INTEGER) FROM meta WHERE key = '"
        LIBRARY_META_GENERATION "'), EXISTS (SELECT 1 FROM track)",
    [ST_ARTISTS] =
        "SELECT ar.id, ar.art_id, (SELECT count(*) FROM album al WHERE al.artist_id = ar.id),"
        " ar.name FROM artist ar ORDER BY ar.sort_name, ar.name, ar.id",
    [ST_ALBUMS] =
        "SELECT " ALBUM_COLS " WHERE al.artist_id = ?1 ORDER BY " ALBUM_ORDER,
    [ST_ALL_ALBUMS] =
        "SELECT " ALBUM_COLS " ORDER BY " ALL_ALBUM_ORDER,
    [ST_TRACKS] =
        "SELECT " TRACK_COLS " FROM track t WHERE t.album_id = ?1 ORDER BY " TRACK_ORDER,
    [ST_ARTIST_TRACKS] =
        "SELECT " TRACK_COLS " FROM track t JOIN album al ON al.id = t.album_id"
        " WHERE al.artist_id = ?1 ORDER BY " ALBUM_ORDER ", " TRACK_ORDER,
    [ST_TRACK_GET] =
        "SELECT t.id, t.album_id, al.artist_id, al.art_id, COALESCE(t.disc_no, 1), t.track_no,"
        " t.duration_ms, t.path, t.title, al.title, ar.name, ar.art_id"
        " FROM track t LEFT JOIN album al ON al.id = t.album_id"
        " LEFT JOIN artist ar ON ar.id = al.artist_id WHERE t.id = ?1",
};

/* List kinds, for the "capped" log (once per list per generation). */
enum { CAP_ARTISTS, CAP_ALBUMS, CAP_ALL_ALBUMS, CAP_TRACKS, CAP_ARTIST_TRACKS, CAP_COUNT };

typedef struct {
    bool    valid;
    int64_t generation;
    int64_t parent;
} cap_log_t;

struct library {
    char           *db_path;
    char           *art_dir;
    sqlite3        *db;
    sqlite3_stmt   *st[ST_COUNT];
    dev_t           dev;            /* identity of the open file */
    ino_t           ino;

    library_state_t state;
    int64_t         generation;     /* -1 without a library */
    uint32_t        epoch;          /* bumped on every usable (re)open */
    bool            opened;         /* opened, epoch not bumped yet */
    char            reason[160];    /* last logged state reason */

    /* What the last poll (or open) reported. */
    library_state_t rep_state;
    int64_t         rep_generation;
    uint32_t        rep_epoch;
    bool            reload_pending; /* a load failed transiently */
    bool            busy_logged;

    /* A load hit a corrupt page: stay in ERROR (without reopening into
     * READY every poll and logging each time) until the generation or the
     * file changes. */
    bool            broken;
    int64_t         broken_generation;
    dev_t           broken_dev;
    ino_t           broken_ino;

    cap_log_t       cap_log[CAP_COUNT];
};

static const int k_art_sizes[LIBRARY_ART_SIZE_COUNT] = LIBRARY_ART_SIZES;

static const char *state_name(library_state_t s)
{
    switch (s) {
    case LIBRARY_MISSING: return "missing";
    case LIBRARY_EMPTY:   return "empty";
    case LIBRARY_READY:   return "ready";
    case LIBRARY_ERROR:   return "error";
    }
    return "?";
}

/* Change state; log only on a transition (or a new reason), never per poll. */
static void set_state(library_t *lib, library_state_t s, const char *reason)
{
    if (s == lib->state && strcmp(reason, lib->reason) == 0)
        return;
    fprintf(stderr, "library: %s (%s)\n", state_name(s), reason);
    lib->state = s;
    snprintf(lib->reason, sizeof(lib->reason), "%s", reason);
    if (s == LIBRARY_MISSING || s == LIBRARY_ERROR)
        lib->generation = -1;
}

static void close_db(library_t *lib)
{
    for (int i = 0; i < ST_COUNT; i++) {
        sqlite3_finalize(lib->st[i]);
        lib->st[i] = NULL;
    }
    if (lib->db) {
        sqlite3_close(lib->db);
        lib->db = NULL;
    }
}

static bool is_transient(int rc)
{
    int prim = rc & 0xff;
    return prim == SQLITE_BUSY || prim == SQLITE_LOCKED;
}

/* A statement failed with `rc`: transient → retry at the next poll; anything
 * else → close and go to ERROR (the next poll reopens and re-validates, but
 * stays in ERROR while the generation and the file are the same). */
static void stmt_failed(library_t *lib, int rc, const char *what)
{
    if (is_transient(rc)) {
        if (!lib->busy_logged) {
            fprintf(stderr, "library: %s: database busy, will retry\n", what);
            lib->busy_logged = true;
        }
        lib->reload_pending = true;
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: %s", what,
             lib->db ? sqlite3_errmsg(lib->db) : sqlite3_errstr(rc));
    lib->broken            = true;
    lib->broken_generation = lib->generation;
    lib->broken_dev        = lib->dev;
    lib->broken_ino        = lib->ino;
    close_db(lib);
    set_state(lib, LIBRARY_ERROR, msg);
}

/* Open the file, check the schema, prepare everything.
 * @return 0 open; 1 busy (closed, try again next poll); -1 failed (closed,
 *         the state says why). */
static int open_db(library_t *lib, const struct stat *st)
{
    char msg[160];
    int rc = sqlite3_open_v2(lib->db_path, &lib->db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        snprintf(msg, sizeof(msg), "open: %s",
                 lib->db ? sqlite3_errmsg(lib->db) : sqlite3_errstr(rc));
        close_db(lib);
        set_state(lib, LIBRARY_ERROR, msg);
        return -1;
    }
    sqlite3_busy_timeout(lib->db, LIBRARY_BUSY_TIMEOUT_MS);

    /* user_version: 0 = the indexer hasn't created the schema yet. */
    sqlite3_stmt *v = NULL;
    rc = sqlite3_prepare_v2(lib->db, "PRAGMA user_version", -1, &v, NULL);
    int ver = -1;
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(v);
        if (rc == SQLITE_ROW) {
            ver = sqlite3_column_int(v, 0);
            rc = SQLITE_OK;
        }
    }
    sqlite3_finalize(v);
    if (rc != SQLITE_OK) {
        if (is_transient(rc)) {     /* try again next poll, quietly */
            close_db(lib);
            return 1;
        }
        /* A read-only connection can't create a missing -wal/-shm pair. */
        snprintf(msg, sizeof(msg), "open: %s%s", sqlite3_errmsg(lib->db),
                 (rc & 0xff) == SQLITE_CANTOPEN ?
                 " (no -wal/-shm next to it: start chatbox-indexer once)" : "");
        close_db(lib);
        set_state(lib, LIBRARY_ERROR, msg);
        return -1;
    }
    if (ver == 0) {
        close_db(lib);
        set_state(lib, LIBRARY_MISSING, "database not initialised yet");
        return -1;
    }
    if (ver != LIBRARY_SCHEMA_VERSION) {
        snprintf(msg, sizeof(msg), "schema version %d, expected %d", ver,
                 LIBRARY_SCHEMA_VERSION);
        close_db(lib);
        set_state(lib, LIBRARY_ERROR, msg);
        return -1;
    }

    for (int i = 0; i < ST_COUNT; i++) {
        rc = sqlite3_prepare_v2(lib->db, k_sql[i], -1, &lib->st[i], NULL);
        if (rc != SQLITE_OK) {
            snprintf(msg, sizeof(msg), "prepare: %s", sqlite3_errmsg(lib->db));
            close_db(lib);
            set_state(lib, LIBRARY_ERROR, msg);
            return -1;
        }
    }
    lib->dev = st->st_dev;
    lib->ino = st->st_ino;
    lib->opened = true;
    return 0;
}

/* Bring state/generation up to date. @return false if the DB was busy (no
 * definitive answer this time). */
static bool refresh(library_t *lib)
{
    struct stat st;
    if (stat(lib->db_path, &st) != 0) {
        close_db(lib);
        if (errno == ENOENT) {
            set_state(lib, LIBRARY_MISSING, "no database yet");
        } else {
            char msg[160];
            snprintf(msg, sizeof(msg), "stat: %s", strerror(errno));
            set_state(lib, LIBRARY_ERROR, msg);
        }
        return true;
    }
    if (lib->db && (st.st_dev != lib->dev || st.st_ino != lib->ino)) {
        fprintf(stderr, "library: database file replaced, reopening\n");
        close_db(lib);
    }
    if (!lib->db) {
        int r = open_db(lib, &st);
        if (r != 0)
            return r < 0;
    }

    sqlite3_stmt *s = lib->st[ST_STATUS];
    int rc = sqlite3_step(s);
    int64_t gen = -1;
    bool has_tracks = false;
    if (rc == SQLITE_ROW) {
        gen = sqlite3_column_type(s, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, 0);
        has_tracks = sqlite3_column_int(s, 1) != 0;
        rc = SQLITE_OK;
    }
    sqlite3_reset(s);
    if (rc != SQLITE_OK) {
        if (is_transient(rc)) {
            if (!lib->busy_logged) {
                fprintf(stderr, "library: poll: database busy, will retry\n");
                lib->busy_logged = true;
            }
            return false;
        }
        stmt_failed(lib, rc, "poll");
        return true;
    }
    lib->busy_logged = false;
    if (lib->broken) {
        if (gen == lib->broken_generation && st.st_dev == lib->broken_dev &&
            st.st_ino == lib->broken_ino) {
            close_db(lib);      /* still the DB that failed: stay in ERROR */
            lib->opened = false;
            return true;
        }
        lib->broken = false;
    }
    if (lib->opened) {          /* a (re)opened DB is always worth a reload */
        lib->epoch++;
        lib->opened = false;
    }
    lib->generation = gen;
    set_state(lib, has_tracks ? LIBRARY_READY : LIBRARY_EMPTY, "database open");
    return true;
}

static void mark_reported(library_t *lib)
{
    lib->rep_state      = lib->state;
    lib->rep_generation = lib->generation;
    lib->rep_epoch      = lib->epoch;
    lib->reload_pending = false;
}

library_t *library_open(const char *db_path, const char *art_dir)
{
    if (!db_path)
        return NULL;
    library_t *lib = calloc(1, sizeof(*lib));
    if (!lib)
        return NULL;
    lib->db_path = strdup(db_path);
    lib->art_dir = art_dir && *art_dir ? strdup(art_dir) : NULL;
    if (!lib->db_path || (art_dir && *art_dir && !lib->art_dir)) {
        library_close(lib);
        return NULL;
    }
    lib->state = LIBRARY_MISSING;
    lib->generation = -1;
    snprintf(lib->reason, sizeof(lib->reason), "not opened");
    if (refresh(lib))
        mark_reported(lib);
    else
        lib->reload_pending = true;     /* busy at boot: the first poll reports */
    return lib;
}

void library_close(library_t *lib)
{
    if (!lib)
        return;
    close_db(lib);
    free(lib->db_path);
    free(lib->art_dir);
    free(lib);
}

bool library_poll(library_t *lib)
{
    if (!lib)
        return false;
    if (!refresh(lib))
        return false;           /* busy: report when it can be read */
    bool changed = lib->reload_pending ||
                   lib->state != lib->rep_state ||
                   lib->generation != lib->rep_generation ||
                   lib->epoch != lib->rep_epoch;
    mark_reported(lib);
    return changed;
}

library_state_t library_state(const library_t *lib)
{
    return lib ? lib->state : LIBRARY_MISSING;
}

int64_t library_generation(const library_t *lib)
{
    return lib ? lib->generation : -1;
}

/* ---------------------------------------------------------------------- */
/* Strings                                                                 */
/* ---------------------------------------------------------------------- */

bool library_utf8_copy(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0)
        return src && *src;
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len < n) {
        memcpy(dst, src, len + 1);
        return false;
    }
    /* src[cut] is the first byte left out. If it continues a code point,
     * back up to that code point's lead byte and leave it out as well. */
    size_t cut = n - 1;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80)
        cut--;
    memcpy(dst, src, cut);
    dst[cut] = '\0';
    return true;
}

/* A name for display: composed (see text_nfc.h), never a path. */
static void col_text(sqlite3_stmt *s, int col, char *dst, size_t n)
{
    library_utf8_copy(dst, n, (const char *)sqlite3_column_text(s, col));
    text_utf8_nfc(dst);
}

static int64_t col_i64(sqlite3_stmt *s, int col, int64_t if_null)
{
    return sqlite3_column_type(s, col) == SQLITE_NULL ? if_null
                                                      : sqlite3_column_int64(s, col);
}

/* ---------------------------------------------------------------------- */
/* Loads                                                                   */
/* ---------------------------------------------------------------------- */

typedef void (*fill_fn)(sqlite3_stmt *s, void *item);

static void fill_artist(sqlite3_stmt *s, void *item)
{
    library_artist_t *a = item;
    a->id          = sqlite3_column_int64(s, 0);
    a->art_id      = col_i64(s, 1, 0);
    a->album_count = sqlite3_column_int(s, 2);
    col_text(s, 3, a->name, sizeof(a->name));
}

static void fill_album(sqlite3_stmt *s, void *item)
{
    library_album_t *a = item;
    a->id          = sqlite3_column_int64(s, 0);
    a->artist_id   = col_i64(s, 1, 0);
    a->art_id      = col_i64(s, 2, 0);
    a->year        = (int)col_i64(s, 3, 0);
    a->track_count = sqlite3_column_int(s, 4);
    col_text(s, 5, a->title, sizeof(a->title));
    col_text(s, 6, a->artist, sizeof(a->artist));
}

static void fill_track(sqlite3_stmt *s, void *item)
{
    library_track_t *t = item;
    t->id          = sqlite3_column_int64(s, 0);
    t->album_id    = col_i64(s, 1, 0);
    t->disc_no     = (int)sqlite3_column_int64(s, 2);
    t->track_no    = (int)col_i64(s, 3, 0);
    t->duration_ms = col_i64(s, 4, -1);
    col_text(s, 5, t->title, sizeof(t->title));
}

/* Run statement `which` (bound to `parent` unless it's < 0) into `items`.
 * Rows past `cap` are counted into *total but not copied. */
static int load(library_t *lib, int which, int kind, int64_t parent,
                void *items, size_t stride, int cap, int *count, int *total,
                fill_fn fill, const char *what)
{
    *count = 0;
    *total = 0;
    if (!lib || !lib->db)
        return LIBRARY_ERR;
    sqlite3_stmt *s = lib->st[which];
    if (parent >= 0)
        sqlite3_bind_int64(s, 1, parent);
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (*count < cap) {
            fill(s, (char *)items + (size_t)*count * stride);
            (*count)++;
        }
        (*total)++;
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    if (rc != SQLITE_DONE) {
        *count = 0;
        *total = 0;
        stmt_failed(lib, rc, what);
        return LIBRARY_ERR;
    }
    if (*total > cap) {
        cap_log_t *c = &lib->cap_log[kind];
        if (!c->valid || c->generation != lib->generation || c->parent != parent) {
            fprintf(stderr, "library: %s", what);
            if (parent >= 0)
                fprintf(stderr, " of %lld", (long long)parent);
            fprintf(stderr, ": %d rows, showing the first %d\n", *total, cap);
            c->valid      = true;
            c->generation = lib->generation;
            c->parent     = parent;
        }
    }
    return LIBRARY_OK;
}

int library_load_artists(library_t *lib, library_artists_t *out)
{
    return load(lib, ST_ARTISTS, CAP_ARTISTS, -1, out->items, sizeof(out->items[0]),
                LIBRARY_MAX_ARTISTS, &out->count, &out->total, fill_artist, "artists");
}

int library_load_albums(library_t *lib, int64_t artist_id, library_albums_t *out)
{
    return load(lib, ST_ALBUMS, CAP_ALBUMS, artist_id < 0 ? 0 : artist_id, out->items,
                sizeof(out->items[0]), LIBRARY_MAX_ALBUMS, &out->count, &out->total,
                fill_album, "albums");
}

int library_load_all_albums(library_t *lib, library_all_albums_t *out)
{
    return load(lib, ST_ALL_ALBUMS, CAP_ALL_ALBUMS, -1, out->items, sizeof(out->items[0]),
                LIBRARY_MAX_ALL_ALBUMS, &out->count, &out->total, fill_album, "all albums");
}

int library_load_tracks(library_t *lib, int64_t album_id, library_tracks_t *out)
{
    return load(lib, ST_TRACKS, CAP_TRACKS, album_id < 0 ? 0 : album_id, out->items,
                sizeof(out->items[0]), LIBRARY_MAX_TRACKS, &out->count, &out->total,
                fill_track, "tracks");
}

int library_load_artist_tracks(library_t *lib, int64_t artist_id,
                               library_artist_tracks_t *out)
{
    return load(lib, ST_ARTIST_TRACKS, CAP_ARTIST_TRACKS, artist_id < 0 ? 0 : artist_id,
                out->items, sizeof(out->items[0]), LIBRARY_MAX_ARTIST_TRACKS,
                &out->count, &out->total, fill_track, "artist tracks");
}

int library_track_get(library_t *lib, int64_t track_id, library_track_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->duration_ms = -1;
    if (!lib || !lib->db)
        return LIBRARY_ERR;
    sqlite3_stmt *s = lib->st[ST_TRACK_GET];
    sqlite3_bind_int64(s, 1, track_id);
    int rc = sqlite3_step(s);
    int ret = LIBRARY_NOT_FOUND;
    if (rc == SQLITE_ROW) {
        out->id          = sqlite3_column_int64(s, 0);
        out->album_id    = col_i64(s, 1, 0);
        out->artist_id   = col_i64(s, 2, 0);
        out->art_id      = col_i64(s, 3, 0);
        out->disc_no     = (int)sqlite3_column_int64(s, 4);
        out->track_no    = (int)col_i64(s, 5, 0);
        out->duration_ms = col_i64(s, 6, -1);
        const char *path = (const char *)sqlite3_column_text(s, 7);
        if (library_utf8_copy(out->path, sizeof(out->path), path)) {
            /* A cut path would name another file: treat it as unusable. */
            fprintf(stderr, "library: track %lld: path too long\n", (long long)track_id);
            out->path[0] = '\0';
        }
        col_text(s, 8, out->title, sizeof(out->title));
        col_text(s, 9, out->album_title, sizeof(out->album_title));
        col_text(s, 10, out->artist_name, sizeof(out->artist_name));
        out->artist_art_id = col_i64(s, 11, 0);
        ret = LIBRARY_OK;
    } else if (rc != SQLITE_DONE) {
        ret = LIBRARY_ERR;
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    if (ret == LIBRARY_ERR) {
        memset(out, 0, sizeof(*out));
        out->duration_ms = -1;
        stmt_failed(lib, rc, "track");
    }
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Art and selection helpers                                               */
/* ---------------------------------------------------------------------- */

bool library_art_path(const library_t *lib, int64_t art_id, int size,
                      char *out, size_t out_size)
{
    if (out && out_size)
        out[0] = '\0';
    if (!lib || !lib->art_dir || art_id <= 0 || !out || out_size == 0)
        return false;
    bool ok_size = false;
    for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++)
        if (k_art_sizes[i] == size)
            ok_size = true;
    if (!ok_size)
        return false;
    int n = snprintf(out, out_size, "%s/%lld_%d.png", lib->art_dir, (long long)art_id, size);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Items of every list start with `int64_t id`. */
static int find(const void *items, size_t stride, int count, int64_t id, int fallback)
{
    if (count <= 0)
        return -1;
    for (int i = 0; i < count; i++)
        if (*(const int64_t *)((const char *)items + (size_t)i * stride) == id)
            return i;
    if (fallback < 0)
        return 0;
    return fallback >= count ? count - 1 : fallback;
}

int library_artists_find(const library_artists_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_albums_find(const library_albums_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_all_albums_find(const library_all_albums_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_tracks_find(const library_tracks_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_artist_tracks_find(const library_artist_tracks_t *l, int64_t id,
                               int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

#ifdef CHATBOX_TESTING
struct sqlite3 *library_test_db(library_t *lib)
{
    return lib ? lib->db : NULL;
}
#endif
