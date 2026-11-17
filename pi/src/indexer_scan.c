/**
 * indexer_scan.c — One indexing pass of chatbox-indexer. See indexer_scan.h.
 *
 * The indexer is its own process (not the LVGL main loop), so the "no
 * dynamic allocation in hot paths" rule doesn't apply: a pass allocates its
 * file lists and frees them at the end.
 */

#include "indexer_scan.h"
#include "library_schema.h"

#include <sqlite3.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WALK_MAX_DEPTH   32
#define DB_BUSY_MS       5000
#define INBOX_DIR        "_inbox"
#define COMPILATIONS_DIR "Compilations"
#define VARIOUS_ARTISTS  "Various Artists"
#define UNKNOWN_ARTIST   "Unknown"
#define INBOX_ALBUM      "Inbox"
#define UNKNOWN_ALBUM    "Unknown"
#define KEY_SEP          '\x1f'
#define EMBEDDED_PREFIX  "embedded:"

static const int k_art_sizes[LIBRARY_ART_SIZE_COUNT] = LIBRARY_ART_SIZES;

/* ---------------------------------------------------------------------- */
/* Types                                                                   */
/* ---------------------------------------------------------------------- */

enum { F_UNCHANGED, F_NEW, F_CHANGED, F_FAILED };

typedef struct {
    char         *path;         /* absolute                                */
    const char   *rel;          /* into path, relative to music_dir        */
    int64_t       mtime_ns;
    int64_t       size;
    int64_t       db_id;        /* existing track row, 0 if none           */
    int           status;
    probe_tags_t *tags;         /* probed this pass (NULL otherwise)       */
} scan_file_t;

typedef struct {
    char   *dir;                /* absolute                                */
    char   *cover;              /* absolute path of the best cover file    */
    int     rank;
    int64_t mtime_ns;
    int64_t size;
} scan_dir_t;

typedef struct {
    int64_t id;
    char   *path;
    int64_t mtime_ns;
    int64_t size;
    bool    seen;
} db_track_t;

/* Hash cache for cover files, so an unchanged cover isn't re-read. */
typedef struct {
    char   *path;
    int64_t mtime_ns;
    int64_t size;
    char    sha1[41];
    bool    bad;                /* thumbnailing failed: don't retry        */
    bool    used;
} art_cache_t;

#define VEC(T) struct { T *v; size_t n, cap; }

typedef struct { dev_t dev; ino_t ino; } dev_ino_t;
typedef VEC(db_track_t) db_tracks_t;

struct indexer {
    sqlite3 *db;
    char    *music_dir;         /* no trailing slash */
    char    *art_dir;
    const indexer_probe_ops_t *ops;
    volatile sig_atomic_t *stop;

    VEC(art_cache_t) cache;

    /* per pass */
    VEC(scan_file_t) files;
    VEC(scan_dir_t)  dirs;
    VEC(char *)      bad_dirs;  /* unreadable: keep their rows            */
    VEC(dev_ino_t)   visited;
    VEC(int64_t)     touched;   /* album ids whose tracks changed          */
    VEC(int64_t)     fields_set;/* album ids whose fields this pass set    */
};

#define VEC_PUSH(vec, item) do {                                           \
    if ((vec).n == (vec).cap) {                                            \
        size_t nc_ = (vec).cap ? (vec).cap * 2 : 16;                       \
        void *nv_ = realloc((vec).v, nc_ * sizeof(*(vec).v));              \
        if (!nv_) { fprintf(stderr, "indexer: out of memory\n"); abort(); }\
        (vec).v = nv_;                                                     \
        (vec).cap = nc_;                                                   \
    }                                                                      \
    (vec).v[(vec).n++] = (item);                                           \
} while (0)

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) {
        fprintf(stderr, "indexer: out of memory\n");
        abort();
    }
    return d;
}

static int64_t stat_mtime_ns(const struct stat *st)
{
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

static bool vec_has_id(const int64_t *v, size_t n, int64_t id)
{
    for (size_t i = 0; i < n; i++)
        if (v[i] == id)
            return true;
    return false;
}

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
        return -1;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static bool has_suffix_ci(const char *s, size_t len, const char *suf)
{
    size_t n = strlen(suf);
    return len >= n && strncasecmp(s + len - n, suf, n) == 0;
}

void indexer_sort_name(const char *name, char *out, size_t out_size)
{
    while (*name == ' ')
        name++;
    if (strncasecmp(name, "the ", 4) == 0 && name[4] != '\0')
        name += 4;
    size_t i = 0;
    for (; name[i] && i + 1 < out_size; i++)
        out[i] = (char)tolower((unsigned char)name[i]);
    out[i] = '\0';
}

bool indexer_is_media_name(const char *name)
{
    static const char *const exts[] = {
        "mp3", "m4a", "aac", "ogg", "oga", "opus", "flac", NULL
    };
    if (name[0] == '.')
        return false;               /* hidden, and "._x" AppleDouble */
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return false;
    bool ok = false;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot + 1, exts[i]) == 0)
            ok = true;
    if (!ok)
        return false;               /* incl. .part / .temp partial files */
    /* Partial files still being written, such as "x.temp.mp3" or "x.part.mp3" */
    size_t stem = (size_t)(dot - name);
    if (has_suffix_ci(name, stem, ".temp") || has_suffix_ci(name, stem, ".part"))
        return false;
    return true;
}

int indexer_cover_rank(const char *name)
{
    static const char *const bases[] = { "cover", "folder", "front" };
    static const char *const exts[]  = { "jpg", "jpeg", "png" };
    const char *dot = strrchr(name, '.');
    if (!dot)
        return -1;
    size_t blen = (size_t)(dot - name);
    for (int b = 0; b < 3; b++) {
        if (blen != strlen(bases[b]) || strncasecmp(name, bases[b], blen) != 0)
            continue;
        for (int e = 0; e < 3; e++)
            if (strcasecmp(dot + 1, exts[e]) == 0)
                return b * 3 + e;
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* SHA-1 (FIPS 180-1), for art dedup                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    size_t   used;
} sha1_t;

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(sha1_t *s)
{
    static const uint32_t h0[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE,
                                    0x10325476, 0xC3D2E1F0 };
    memcpy(s->h, h0, sizeof(h0));
    s->len = 0;
    s->used = 0;
}

static void sha1_update(sha1_t *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->used;
        if (take > n)
            take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha1_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha1_final(sha1_t *s, char out[41])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56)
        sha1_update(s, &pad, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++)
        snprintf(out + i * 8, 9, "%08x", s->h[i]);
}

int indexer_sha1_file(const char *path, char out[41])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha1_t s;
    sha1_init(&s);
    uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha1_update(&s, buf, n);
    bool err = ferror(f);
    fclose(f);
    if (err)
        return -1;
    sha1_final(&s, out);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* SQLite helpers                                                          */
/* ---------------------------------------------------------------------- */

static int db_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "indexer: sql '%.60s': %s\n", sql, err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static sqlite3_stmt *db_prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "indexer: prepare '%.60s': %s\n", sql, sqlite3_errmsg(db));
        return NULL;
    }
    return st;
}

/* Step a statement that returns no rows; resets it. 0 ok, -1 error. */
static int db_done(sqlite3 *db, sqlite3_stmt *st)
{
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "indexer: sql step: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static void bind_text(sqlite3_stmt *st, int i, const char *s)
{
    sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
}

static void bind_int_or_null(sqlite3_stmt *st, int i, int64_t v)
{
    if (v > 0)
        sqlite3_bind_int64(st, i, v);
    else
        sqlite3_bind_null(st, i);
}

/* ---------------------------------------------------------------------- */
/* Open / close                                                            */
/* ---------------------------------------------------------------------- */

static int db_init_schema(sqlite3 *db)
{
    sqlite3_stmt *st = db_prep(db, "PRAGMA user_version");
    if (!st)
        return -1;
    int ver = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);

    if (ver == LIBRARY_SCHEMA_VERSION)
        return 0;
    if (ver != 0) {
        fprintf(stderr, "indexer: DB schema version %d, expected %d; "
                "delete the DB to rebuild it\n", ver, LIBRARY_SCHEMA_VERSION);
        return -1;
    }

    char sql[256];
    if (db_exec(db, "BEGIN IMMEDIATE") != 0)
        return -1;
    snprintf(sql, sizeof(sql),
             "INSERT INTO meta(key, value) VALUES ('" LIBRARY_META_SCHEMA_VERSION "', '%d'),"
             " ('" LIBRARY_META_GENERATION "', '0'), ('" LIBRARY_META_LAST_SCAN "', '')",
             LIBRARY_SCHEMA_VERSION);
    char pragma[64];
    snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d", LIBRARY_SCHEMA_VERSION);
    if (db_exec(db, LIBRARY_SCHEMA_SQL) != 0 || db_exec(db, sql) != 0 ||
        db_exec(db, pragma) != 0) {
        db_exec(db, "ROLLBACK");
        return -1;
    }
    return db_exec(db, "COMMIT");
}

indexer_t *indexer_open(const indexer_opts_t *o)
{
    indexer_t *ix = calloc(1, sizeof(*ix));
    if (!ix)
        return NULL;
    ix->music_dir = xstrdup(o->music_dir);
    size_t n = strlen(ix->music_dir);
    while (n > 1 && ix->music_dir[n - 1] == '/')
        ix->music_dir[--n] = '\0';
    ix->art_dir = xstrdup(o->art_dir);
    ix->ops     = o->ops;
    ix->stop    = o->stop;

    if (mkdir_p(ix->art_dir) != 0) {
        fprintf(stderr, "indexer: cannot create art dir '%s': %s\n",
                ix->art_dir, strerror(errno));
        goto fail;
    }

    if (sqlite3_open_v2(o->db_path, &ix->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        fprintf(stderr, "indexer: cannot open DB '%s': %s\n", o->db_path,
                ix->db ? sqlite3_errmsg(ix->db) : "?");
        goto fail;
    }
    /* A UI reader never blocks a WAL writer; the timeout covers checkpoints
     * and a stray second writer. */
    sqlite3_busy_timeout(ix->db, DB_BUSY_MS);

    /* Keep -wal/-shm when the last connection closes. A SQLITE_OPEN_READONLY
     * reader (the UI, §2) can't create them, so without this the UI can't
     * read the library while the indexer is stopped (--once, restart). */
    int persist = 1;
    sqlite3_file_control(ix->db, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);

    sqlite3_stmt *st = db_prep(ix->db, "PRAGMA journal_mode=WAL");
    const char *mode = st && sqlite3_step(st) == SQLITE_ROW ?
                       (const char *)sqlite3_column_text(st, 0) : NULL;
    bool wal = mode && strcasecmp(mode, "wal") == 0;
    sqlite3_finalize(st);
    if (!wal) {
        fprintf(stderr, "indexer: cannot enable WAL on '%s'\n", o->db_path);
        goto fail;
    }
    if (db_exec(ix->db, "PRAGMA synchronous=NORMAL") != 0 ||
        db_init_schema(ix->db) != 0)
        goto fail;
    return ix;

fail:
    indexer_close(ix);
    return NULL;
}

static void pass_reset(indexer_t *ix)
{
    for (size_t i = 0; i < ix->files.n; i++) {
        free(ix->files.v[i].path);
        free(ix->files.v[i].tags);
    }
    for (size_t i = 0; i < ix->dirs.n; i++) {
        free(ix->dirs.v[i].dir);
        free(ix->dirs.v[i].cover);
    }
    for (size_t i = 0; i < ix->bad_dirs.n; i++)
        free(ix->bad_dirs.v[i]);
    ix->files.n = ix->dirs.n = ix->bad_dirs.n = 0;
    ix->visited.n = ix->touched.n = ix->fields_set.n = 0;
}

void indexer_close(indexer_t *ix)
{
    if (!ix)
        return;
    pass_reset(ix);
    free(ix->files.v);
    free(ix->dirs.v);
    free(ix->bad_dirs.v);
    free(ix->visited.v);
    free(ix->touched.v);
    free(ix->fields_set.v);
    for (size_t i = 0; i < ix->cache.n; i++)
        free(ix->cache.v[i].path);
    free(ix->cache.v);
    if (ix->db)
        sqlite3_close(ix->db);
    free(ix->music_dir);
    free(ix->art_dir);
    free(ix);
}

/* ---------------------------------------------------------------------- */
/* Walk                                                                    */
/* ---------------------------------------------------------------------- */

static bool stop_requested(const indexer_t *ix)
{
    return ix->stop && *ix->stop;
}

static void walk(indexer_t *ix, const char *dir, int depth)
{
    if (depth > WALK_MAX_DEPTH) {
        fprintf(stderr, "indexer: '%s' too deep, skipped\n", dir);
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "indexer: cannot read '%s': %s (keeping its tracks)\n",
                dir, strerror(errno));
        VEC_PUSH(ix->bad_dirs, xstrdup(dir));
        return;
    }

    scan_dir_t sd = { .dir = NULL, .cover = NULL, .rank = 99 };
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.')
            continue;           /* ., .., hidden, AppleDouble, rsync temps */

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0)
            continue;           /* dangling symlink, raced delete */

        if (S_ISDIR(st.st_mode)) {
            bool seen = false;
            for (size_t i = 0; i < ix->visited.n; i++)
                if (ix->visited.v[i].dev == st.st_dev && ix->visited.v[i].ino == st.st_ino)
                    seen = true;
            if (seen)
                continue;       /* symlink loop or second link to a dir */
            dev_ino_t vi = { st.st_dev, st.st_ino };
            VEC_PUSH(ix->visited, vi);
            walk(ix, path, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;

        if (indexer_is_media_name(name)) {
            scan_file_t f = {
                .path = xstrdup(path), .mtime_ns = stat_mtime_ns(&st),
                .size = (int64_t)st.st_size, .status = F_UNCHANGED,
            };
            f.rel = f.path + strlen(ix->music_dir) + 1;
            VEC_PUSH(ix->files, f);
            continue;
        }
        int rank = indexer_cover_rank(name);
        if (rank >= 0 && rank < sd.rank && st.st_size > 0) {
            free(sd.cover);
            sd.cover    = xstrdup(path);
            sd.rank     = rank;
            sd.mtime_ns = stat_mtime_ns(&st);
            sd.size     = (int64_t)st.st_size;
        }
    }
    closedir(d);

    if (sd.cover) {
        sd.dir = xstrdup(dir);
        VEC_PUSH(ix->dirs, sd);
    }
}

static int cmp_file(const void *a, const void *b)
{
    return strcmp(((const scan_file_t *)a)->path, ((const scan_file_t *)b)->path);
}

static scan_file_t *find_file(indexer_t *ix, const char *path)
{
    scan_file_t key = { .path = (char *)path };
    return bsearch(&key, ix->files.v, ix->files.n, sizeof(scan_file_t), cmp_file);
}

static const scan_dir_t *find_dir(const indexer_t *ix, const char *dir)
{
    for (size_t i = 0; i < ix->dirs.n; i++)
        if (strcmp(ix->dirs.v[i].dir, dir) == 0)
            return &ix->dirs.v[i];
    return NULL;
}

static bool under_bad_dir(const indexer_t *ix, const char *path)
{
    for (size_t i = 0; i < ix->bad_dirs.n; i++) {
        size_t n = strlen(ix->bad_dirs.v[i]);
        if (strncmp(path, ix->bad_dirs.v[i], n) == 0 && path[n] == '/')
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------------- */
/* Tags + path → album / track fields                                      */
/* ---------------------------------------------------------------------- */

typedef struct {
    char    key[PATH_MAX + 2 * PROBE_STR_LEN];  /* album.dir */
    char    artist[PROBE_STR_LEN];
    char    title[PROBE_STR_LEN];
    int     year;
    char    track_title[PROBE_STR_LEN];
    int     track_no;
    int     disc_no;
} derived_t;

/* "07 - Title" → 7, "Title"; "Title" → 0, "Title". */
static void split_number(const char *stem, int *num, char *title, size_t n)
{
    const char *p = stem;
    int v = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) {
        v = v * 10 + (*p - '0');
        p++;
        digits++;
    }
    const char *rest = p;
    while (*rest == ' ' || *rest == '.' || *rest == '-' || *rest == '_')
        rest++;
    if (digits == 0 || (*p >= '0' && *p <= '9') || *rest == '\0' ||
        (rest == p && *p != '\0')) {
        /* no number, a long number, nothing after it, or "3rd Song" */
        *num = 0;
        snprintf(title, n, "%s", stem);
        return;
    }
    *num = v;
    snprintf(title, n, "%s", rest);
}

static void derive(const indexer_t *ix, const scan_file_t *f, derived_t *d)
{
    const probe_tags_t *t = f->tags;
    memset(d, 0, sizeof(*d));

    /* Split rel into directory components and the file name. */
    char rel[PATH_MAX];
    snprintf(rel, sizeof(rel), "%s", f->rel);
    const char *comp[WALK_MAX_DEPTH + 2];
    int n = 0;
    for (char *p = rel, *s; (s = strsep(&p, "/")) != NULL && n < WALK_MAX_DEPTH + 2;)
        comp[n++] = s;
    const char *file   = comp[n - 1];
    const char *parent = n >= 2 ? comp[n - 2] : NULL;
    const char *grand  = n >= 3 ? comp[n - 3] : NULL;
    bool inbox = n >= 2 && strcasecmp(comp[0], INBOX_DIR) == 0;
    bool compil = n >= 2 && strcasecmp(comp[0], COMPILATIONS_DIR) == 0;

    /* Album artist */
    if (compil)
        snprintf(d->artist, sizeof(d->artist), "%s", VARIOUS_ARTISTS);
    else if (t->album_artist[0])
        snprintf(d->artist, sizeof(d->artist), "%s", t->album_artist);
    else if (t->artist[0])
        snprintf(d->artist, sizeof(d->artist), "%s", t->artist);
    else if (inbox || n < 2)
        snprintf(d->artist, sizeof(d->artist), "%s", UNKNOWN_ARTIST);
    else
        snprintf(d->artist, sizeof(d->artist), "%s", grand ? grand : parent);

    /* Album title */
    if (t->album[0])
        snprintf(d->title, sizeof(d->title), "%s", t->album);
    else if (inbox && n == 2)
        snprintf(d->title, sizeof(d->title), "%s", INBOX_ALBUM);
    else
        snprintf(d->title, sizeof(d->title), "%s", parent ? parent : UNKNOWN_ALBUM);
    d->year = t->year;

    /* Track */
    char stem[PROBE_STR_LEN];
    snprintf(stem, sizeof(stem), "%s", file);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = '\0';
    int fnum = 0;
    char ftitle[PROBE_STR_LEN];
    split_number(stem, &fnum, ftitle, sizeof(ftitle));
    snprintf(d->track_title, sizeof(d->track_title), "%s", t->title[0] ? t->title : ftitle);
    d->track_no = t->track_no > 0 ? t->track_no : fnum;
    d->disc_no  = t->disc_no > 0 ? t->disc_no : 1;

    /* Album identity: the directory; per tag group inside _inbox. */
    const char *slash = strrchr(f->rel, '/');
    int dlen = slash ? (int)(slash - f->rel) : 0;
    if (dlen == 0)
        snprintf(d->key, sizeof(d->key), ".");
    else
        snprintf(d->key, sizeof(d->key), "%.*s", dlen, f->rel);
    if (inbox) {
        size_t k = strlen(d->key);
        snprintf(d->key + k, sizeof(d->key) - k, "%c%s%c%s",
                 KEY_SEP, d->artist, KEY_SEP, d->title);
    }
    (void)ix;
}

/* ---------------------------------------------------------------------- */
/* The pass                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    sqlite3_stmt *artist_get, *artist_ins;
    sqlite3_stmt *album_get, *album_ins, *album_upd;
    sqlite3_stmt *track_ins, *track_upd, *track_del, *track_album;
    sqlite3_stmt *art_by_sha, *art_ins, *album_set_art;
} stmts_t;

static void stmts_free(stmts_t *s)
{
    sqlite3_stmt **p = (sqlite3_stmt **)s;
    for (size_t i = 0; i < sizeof(*s) / sizeof(*p); i++)
        sqlite3_finalize(p[i]);
    memset(s, 0, sizeof(*s));
}

static int stmts_init(sqlite3 *db, stmts_t *s)
{
    memset(s, 0, sizeof(*s));
    s->artist_get = db_prep(db, "SELECT id FROM artist WHERE name = ?1");
    s->artist_ins = db_prep(db, "INSERT INTO artist(name, sort_name) VALUES (?1, ?2)");
    s->album_get  = db_prep(db, "SELECT id FROM album WHERE dir = ?1");
    s->album_ins  = db_prep(db, "INSERT INTO album(artist_id, title, year, dir) VALUES (?1, ?2, ?3, ?4)");
    s->album_upd  = db_prep(db, "UPDATE album SET artist_id = ?1, title = ?2, year = ?3 WHERE id = ?4 "
                                "AND (artist_id IS NOT ?1 OR title IS NOT ?2 OR year IS NOT ?3)");
    s->track_ins  = db_prep(db, "INSERT INTO track(album_id, path, title, track_no, disc_no,"
                                " duration_ms, mtime, size) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    s->track_upd  = db_prep(db, "UPDATE track SET album_id = ?1, title = ?3, track_no = ?4, disc_no = ?5,"
                                " duration_ms = ?6, mtime = ?7, size = ?8 WHERE id = ?9");
    s->track_del  = db_prep(db, "DELETE FROM track WHERE id = ?1");
    s->track_album = db_prep(db, "SELECT album_id FROM track WHERE id = ?1");
    s->art_by_sha = db_prep(db, "SELECT id FROM art WHERE sha1 = ?1");
    s->art_ins    = db_prep(db, "INSERT INTO art(sha1, src) VALUES (?1, ?2)");
    s->album_set_art = db_prep(db, "UPDATE album SET art_id = ?1 WHERE id = ?2 AND art_id IS NOT ?1");

    sqlite3_stmt **p = (sqlite3_stmt **)s;
    for (size_t i = 0; i < sizeof(*s) / sizeof(*p); i++)
        if (!p[i]) {
            stmts_free(s);
            return -1;
        }
    return 0;
}

static int64_t single_id(sqlite3 *db, sqlite3_stmt *st, int *err)
{
    int rc = sqlite3_step(st);
    int64_t id = 0;
    if (rc == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    else if (rc != SQLITE_DONE) {
        fprintf(stderr, "indexer: sql: %s\n", sqlite3_errmsg(db));
        *err = 1;
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return id;
}

static void touch_album(indexer_t *ix, int64_t id)
{
    if (id > 0 && !vec_has_id(ix->touched.v, ix->touched.n, id))
        VEC_PUSH(ix->touched, id);
}

static int64_t get_artist(indexer_t *ix, stmts_t *s, const char *name, int *err)
{
    bind_text(s->artist_get, 1, name);
    int64_t id = single_id(ix->db, s->artist_get, err);
    if (id || *err)
        return id;
    char sort[PROBE_STR_LEN];
    indexer_sort_name(name, sort, sizeof(sort));
    bind_text(s->artist_ins, 1, name);
    bind_text(s->artist_ins, 2, sort);
    if (db_done(ix->db, s->artist_ins) != 0) {
        *err = 1;
        return 0;
    }
    return sqlite3_last_insert_rowid(ix->db);
}

/* Write one probed file. */
static int write_track(indexer_t *ix, stmts_t *s, scan_file_t *f)
{
    derived_t d;
    derive(ix, f, &d);
    int err = 0;

    int64_t artist_id = get_artist(ix, s, d.artist, &err);
    if (err)
        return -1;

    bind_text(s->album_get, 1, d.key);
    int64_t album_id = single_id(ix->db, s->album_get, &err);
    if (err)
        return -1;
    if (!album_id) {
        sqlite3_bind_int64(s->album_ins, 1, artist_id);
        bind_text(s->album_ins, 2, d.title);
        bind_int_or_null(s->album_ins, 3, d.year);
        bind_text(s->album_ins, 4, d.key);
        if (db_done(ix->db, s->album_ins) != 0)
            return -1;
        album_id = sqlite3_last_insert_rowid(ix->db);
        VEC_PUSH(ix->fields_set, album_id);
    } else if (!vec_has_id(ix->fields_set.v, ix->fields_set.n, album_id)) {
        /* The first track of this album probed in this pass sets its fields. */
        sqlite3_bind_int64(s->album_upd, 1, artist_id);
        bind_text(s->album_upd, 2, d.title);
        bind_int_or_null(s->album_upd, 3, d.year);
        sqlite3_bind_int64(s->album_upd, 4, album_id);
        if (db_done(ix->db, s->album_upd) != 0)
            return -1;
        VEC_PUSH(ix->fields_set, album_id);
    }
    touch_album(ix, album_id);

    sqlite3_stmt *st = f->db_id ? s->track_upd : s->track_ins;
    if (f->db_id) {
        sqlite3_bind_int64(s->track_album, 1, f->db_id);
        touch_album(ix, single_id(ix->db, s->track_album, &err));  /* old album */
        if (err)
            return -1;
        sqlite3_bind_int64(st, 9, f->db_id);
    }
    sqlite3_bind_int64(st, 1, album_id);
    bind_text(st, 2, f->path);
    bind_text(st, 3, d.track_title);
    bind_int_or_null(st, 4, d.track_no);
    bind_int_or_null(st, 5, d.disc_no);
    if (f->tags->duration_ms >= 0)
        sqlite3_bind_int64(st, 6, f->tags->duration_ms);
    else
        sqlite3_bind_null(st, 6);
    sqlite3_bind_int64(st, 7, f->mtime_ns);
    sqlite3_bind_int64(st, 8, f->size);
    return db_done(ix->db, st);
}

/* ---- art ---- */

static void thumb_path(const indexer_t *ix, char *out, size_t n, int64_t id, int size)
{
    snprintf(out, n, "%s/%lld_%d.png", ix->art_dir, (long long)id, size);
}

static void tmp_path(const indexer_t *ix, char *out, size_t n, const char *what)
{
    snprintf(out, n, "%s/.tmp-%s", ix->art_dir, what);
}

static bool thumbs_exist(const indexer_t *ix, int64_t id)
{
    for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++) {
        char p[PATH_MAX];
        struct stat st;
        thumb_path(ix, p, sizeof(p), id, k_art_sizes[i]);
        if (stat(p, &st) != 0 || st.st_size == 0)
            return false;
    }
    return true;
}

/* Make all thumbnails of `image` under temp names. 0 ok. */
static int make_thumbs_tmp(indexer_t *ix, const char *image, const char *sha1)
{
    for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++) {
        char what[64], p[PATH_MAX];
        snprintf(what, sizeof(what), "%s_%d.png", sha1, k_art_sizes[i]);
        tmp_path(ix, p, sizeof(p), what);
        struct stat st;
        if (ix->ops->thumbnail(ix->ops->ctx, image, k_art_sizes[i], p) != 0 ||
            stat(p, &st) != 0 || st.st_size == 0)
            return -1;
    }
    return 0;
}

/* Rename the temp thumbnails of `sha1` into place for art `id`. 0 ok. */
static int publish_thumbs(indexer_t *ix, const char *sha1, int64_t id)
{
    for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++) {
        char what[64], from[PATH_MAX], to[PATH_MAX];
        snprintf(what, sizeof(what), "%s_%d.png", sha1, k_art_sizes[i]);
        tmp_path(ix, from, sizeof(from), what);
        thumb_path(ix, to, sizeof(to), id, k_art_sizes[i]);
        if (rename(from, to) != 0) {
            fprintf(stderr, "indexer: rename '%s': %s\n", to, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* Find or create the art row for image file `image` with hash `sha1`,
 * making its thumbnails if needed. Returns the art id, 0 when the image is
 * unusable, -1 on a DB error. */
static int64_t art_for_image(indexer_t *ix, stmts_t *s, const char *image,
                             const char *sha1, const char *src)
{
    int err = 0;
    bind_text(s->art_by_sha, 1, sha1);
    int64_t id = single_id(ix->db, s->art_by_sha, &err);
    if (err)
        return -1;
    if (id && thumbs_exist(ix, id))
        return id;

    if (make_thumbs_tmp(ix, image, sha1) != 0) {
        fprintf(stderr, "indexer: cannot make thumbnails of '%s'\n", src);
        return 0;
    }
    if (!id) {
        bind_text(s->art_ins, 1, sha1);
        bind_text(s->art_ins, 2, src);
        if (db_done(ix->db, s->art_ins) != 0)
            return -1;
        id = sqlite3_last_insert_rowid(ix->db);
    }
    /* Before the commit that references `id` (§2). */
    if (publish_thumbs(ix, sha1, id) != 0)
        return -1;
    return id;
}

static art_cache_t *cache_get(indexer_t *ix, const scan_dir_t *sd)
{
    for (size_t i = 0; i < ix->cache.n; i++) {
        art_cache_t *c = &ix->cache.v[i];
        if (strcmp(c->path, sd->cover) == 0) {
            if (c->mtime_ns != sd->mtime_ns || c->size != sd->size) {
                c->mtime_ns = sd->mtime_ns;
                c->size     = sd->size;
                c->sha1[0]  = '\0';
                c->bad      = false;
            }
            c->used = true;
            return c;
        }
    }
    art_cache_t c = { .path = xstrdup(sd->cover), .mtime_ns = sd->mtime_ns,
                      .size = sd->size, .used = true };
    VEC_PUSH(ix->cache, c);
    return &ix->cache.v[ix->cache.n - 1];
}

/* Art from a cover file: id, 0 if unusable, -1 on DB error. */
static int64_t art_from_cover(indexer_t *ix, stmts_t *s, const scan_dir_t *sd)
{
    art_cache_t *c = cache_get(ix, sd);
    if (c->bad)
        return 0;
    if (!c->sha1[0] && indexer_sha1_file(sd->cover, c->sha1) != 0) {
        c->sha1[0] = '\0';
        return 0;
    }
    int64_t id = art_for_image(ix, s, sd->cover, c->sha1,
                               sd->cover + strlen(ix->music_dir) + 1);
    if (id == 0)
        c->bad = true;
    return id;
}

/* Embedded art from the album's first track that has it: id, 0, -1. */
static int64_t art_embedded(indexer_t *ix, stmts_t *s, int64_t album_id)
{
    sqlite3_stmt *q = db_prep(ix->db, "SELECT path FROM track WHERE album_id = ?1 "
                                      "ORDER BY disc_no, track_no, path");
    if (!q)
        return -1;
    sqlite3_bind_int64(q, 1, album_id);

    char tmp[PATH_MAX];
    tmp_path(ix, tmp, sizeof(tmp), "extract");
    int64_t id = 0;
    int rc;
    while (id == 0 && (rc = sqlite3_step(q)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(q, 0);
        const scan_file_t *f = find_file(ix, path);
        if (f && f->tags && !f->tags->has_art)
            continue;   /* probed this pass: known to have none */
        unlink(tmp);
        if (ix->ops->extract_art(ix->ops->ctx, path, tmp) != 0)
            continue;
        char sha1[41];
        if (indexer_sha1_file(tmp, sha1) != 0)
            continue;
        char src[PATH_MAX];
        snprintf(src, sizeof(src), EMBEDDED_PREFIX "%s", path + strlen(ix->music_dir) + 1);
        id = art_for_image(ix, s, tmp, sha1, src);
    }
    sqlite3_finalize(q);
    unlink(tmp);
    return id;
}

/* Resolve art for every album (§3 order). 0 ok, -1 DB error. */
static int resolve_album_art(indexer_t *ix, stmts_t *s)
{
    sqlite3_stmt *q = db_prep(ix->db,
        "SELECT album.id, album.dir, album.art_id, art.src FROM album "
        "LEFT JOIN art ON art.id = album.art_id");
    if (!q)
        return -1;

    typedef struct { int64_t id, art_id; char *dir; bool art_embedded; } row_t;
    VEC(row_t) rows = { 0 };
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *src = (const char *)sqlite3_column_text(q, 3);
        row_t r = {
            .id = sqlite3_column_int64(q, 0),
            .art_id = sqlite3_column_int64(q, 2),
            .dir = xstrdup((const char *)sqlite3_column_text(q, 1)),
            .art_embedded = src && strncmp(src, EMBEDDED_PREFIX, strlen(EMBEDDED_PREFIX)) == 0,
        };
        VEC_PUSH(rows, r);
    }
    sqlite3_finalize(q);

    int rc = 0;
    for (size_t i = 0; i < rows.n && rc == 0; i++) {
        row_t *r = &rows.v[i];
        char *sep = strchr(r->dir, KEY_SEP);
        if (sep)
            *sep = '\0';
        char abs[PATH_MAX];
        if (strcmp(r->dir, ".") == 0)
            snprintf(abs, sizeof(abs), "%s", ix->music_dir);
        else
            snprintf(abs, sizeof(abs), "%s/%s", ix->music_dir, r->dir);

        const scan_dir_t *sd = find_dir(ix, abs);
        int64_t want = sd ? art_from_cover(ix, s, sd) : 0;
        if (want < 0) {
            rc = -1;
            break;
        }
        if (want == 0) {
            bool touched = vec_has_id(ix->touched.v, ix->touched.n, r->id);
            bool cover_gone = r->art_id && !r->art_embedded;
            if (touched || cover_gone)
                want = art_embedded(ix, s, r->id);
            else
                want = r->art_id;       /* unchanged album: keep */
            if (want < 0) {
                rc = -1;
                break;
            }
        }
        if (want)
            sqlite3_bind_int64(s->album_set_art, 1, want);
        else
            sqlite3_bind_null(s->album_set_art, 1);
        sqlite3_bind_int64(s->album_set_art, 2, r->id);
        rc = db_done(ix->db, s->album_set_art);
    }
    for (size_t i = 0; i < rows.n; i++)
        free(rows.v[i].dir);
    free(rows.v);
    return rc;
}

/* After the pass: delete thumbnails and temp files no art row references. */
static void cleanup_art_dir(indexer_t *ix)
{
    VEC(int64_t) ids = { 0 };
    sqlite3_stmt *q = db_prep(ix->db, "SELECT id FROM art");
    if (!q)
        return;
    while (sqlite3_step(q) == SQLITE_ROW)
        VEC_PUSH(ids, sqlite3_column_int64(q, 0));
    sqlite3_finalize(q);

    DIR *d = opendir(ix->art_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *name = de->d_name;
            bool del = false;
            long long id;
            int size, used = 0;
            if (strncmp(name, ".tmp-", 5) == 0) {
                del = true;
            } else if (sscanf(name, "%lld_%d.png%n", &id, &size, &used) == 2 &&
                       name[used] == '\0') {
                char canon[64];
                snprintf(canon, sizeof(canon), "%lld_%d.png", id, size);
                del = strcmp(canon, name) == 0 && !vec_has_id(ids.v, ids.n, id);
            }
            if (del) {
                char p[PATH_MAX];
                snprintf(p, sizeof(p), "%s/%s", ix->art_dir, name);
                unlink(p);
            }
        }
        closedir(d);
    }
    free(ids.v);

    /* Drop cache entries for covers that weren't seen this pass. */
    size_t w = 0;
    for (size_t i = 0; i < ix->cache.n; i++) {
        if (ix->cache.v[i].used) {
            ix->cache.v[i].used = false;
            ix->cache.v[w++] = ix->cache.v[i];
        } else {
            free(ix->cache.v[i].path);
        }
    }
    ix->cache.n = w;
}

static int64_t read_generation(sqlite3 *db)
{
    int64_t g = 0;
    sqlite3_stmt *q = db_prep(db, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = '"
                                  LIBRARY_META_GENERATION "'");
    if (q && sqlite3_step(q) == SQLITE_ROW)
        g = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return g;
}

/* Load the track rows (sorted by path, same order as strcmp) and match them
 * with the walked files. */
static int match_rows(indexer_t *ix, db_tracks_t *rows)
{
    sqlite3_stmt *q = db_prep(ix->db, "SELECT id, path, mtime, size FROM track ORDER BY path");
    if (!q)
        return -1;
    while (sqlite3_step(q) == SQLITE_ROW) {
        db_track_t r = {
            .id = sqlite3_column_int64(q, 0),
            .path = xstrdup((const char *)sqlite3_column_text(q, 1)),
            .mtime_ns = sqlite3_column_int64(q, 2),
            .size = sqlite3_column_int64(q, 3),
        };
        VEC_PUSH(*rows, r);
    }
    sqlite3_finalize(q);

    size_t j = 0;
    for (size_t i = 0; i < ix->files.n; i++) {
        scan_file_t *f = &ix->files.v[i];
        while (j < rows->n && strcmp(rows->v[j].path, f->path) < 0)
            j++;
        if (j < rows->n && strcmp(rows->v[j].path, f->path) == 0) {
            db_track_t *r = &rows->v[j];
            r->seen  = true;
            f->db_id = r->id;
            f->status = (r->mtime_ns == f->mtime_ns && r->size == f->size) ?
                        F_UNCHANGED : F_CHANGED;
        } else {
            f->status = F_NEW;
        }
    }
    for (size_t k = 0; k < rows->n; k++)
        if (!rows->v[k].seen && under_bad_dir(ix, rows->v[k].path))
            rows->v[k].seen = true;     /* unreadable dir: keep */
    return 0;
}

int indexer_pass(indexer_t *ix, indexer_stats_t *stats)
{
    indexer_stats_t local;
    if (!stats)
        stats = &local;
    memset(stats, 0, sizeof(*stats));
    pass_reset(ix);

    struct stat rst;
    if (stat(ix->music_dir, &rst) != 0 || !S_ISDIR(rst.st_mode)) {
        /* Never wipe the library because the music dir is missing
         * (unmounted disk, typo): abort the pass instead. */
        fprintf(stderr, "indexer: music dir '%s' missing, pass skipped\n", ix->music_dir);
        return -1;
    }
    dev_ino_t root = { rst.st_dev, rst.st_ino };
    VEC_PUSH(ix->visited, root);
    walk(ix, ix->music_dir, 0);
    if (ix->bad_dirs.n > 0 && strcmp(ix->bad_dirs.v[0], ix->music_dir) == 0) {
        fprintf(stderr, "indexer: music dir unreadable, pass skipped\n");
        return -1;
    }
    qsort(ix->files.v, ix->files.n, sizeof(scan_file_t), cmp_file);
    stats->files = (int)ix->files.n;

    db_tracks_t rows = { 0 };
    stmts_t s = { 0 };
    bool in_txn = false;
    int rc = -1;

    if (match_rows(ix, &rows) != 0)
        goto out;

    /* Probe new/changed files, outside the transaction. */
    for (size_t i = 0; i < ix->files.n; i++) {
        scan_file_t *f = &ix->files.v[i];
        if (f->status == F_UNCHANGED)
            continue;
        if (stop_requested(ix))
            goto out;
        if (f->size == 0) {
            f->status = F_FAILED;       /* being written */
            stats->probe_failed++;
            continue;
        }
        f->tags = calloc(1, sizeof(*f->tags));
        if (!f->tags)
            goto out;
        stats->probed++;
        if (ix->ops->probe(ix->ops->ctx, f->path, f->tags) != 0) {
            fprintf(stderr, "indexer: cannot read tags of '%s', skipped until next pass\n",
                    f->rel);
            free(f->tags);
            f->tags = NULL;
            f->status = F_FAILED;       /* a changed file keeps its old row */
            stats->probe_failed++;
        }
    }
    if (stop_requested(ix))
        goto out;

    if (stmts_init(ix->db, &s) != 0 || db_exec(ix->db, "BEGIN IMMEDIATE") != 0)
        goto out;
    in_txn = true;
    int before = sqlite3_total_changes(ix->db);

    /* Vanished files */
    for (size_t k = 0; k < rows.n; k++) {
        if (rows.v[k].seen)
            continue;
        int err = 0;
        sqlite3_bind_int64(s.track_album, 1, rows.v[k].id);
        touch_album(ix, single_id(ix->db, s.track_album, &err));
        sqlite3_bind_int64(s.track_del, 1, rows.v[k].id);
        if (err || db_done(ix->db, s.track_del) != 0)
            goto out;
        stats->removed++;
    }

    /* New / changed files */
    for (size_t i = 0; i < ix->files.n; i++) {
        scan_file_t *f = &ix->files.v[i];
        if (f->status != F_NEW && f->status != F_CHANGED)
            continue;
        if (write_track(ix, &s, f) != 0)
            goto out;
        if (f->status == F_NEW)
            stats->added++;
        else
            stats->updated++;
    }

    /* Empty albums, then artists without albums. */
    if (db_exec(ix->db, "DELETE FROM album WHERE id NOT IN "
                        "(SELECT album_id FROM track WHERE album_id IS NOT NULL)") != 0 ||
        db_exec(ix->db, "DELETE FROM artist WHERE id NOT IN "
                        "(SELECT artist_id FROM album WHERE artist_id IS NOT NULL)") != 0)
        goto out;

    if (resolve_album_art(ix, &s) != 0)
        goto out;

    /* Artist art = the newest album (by year, then most recently added)
     * that has art; unreferenced art rows go. */
    if (db_exec(ix->db,
            "UPDATE artist SET art_id = (SELECT a.art_id FROM album a WHERE a.artist_id = artist.id"
            " AND a.art_id IS NOT NULL ORDER BY a.year IS NULL, a.year DESC, a.id DESC LIMIT 1)"
            " WHERE art_id IS NOT (SELECT a.art_id FROM album a WHERE a.artist_id = artist.id"
            " AND a.art_id IS NOT NULL ORDER BY a.year IS NULL, a.year DESC, a.id DESC LIMIT 1)") != 0 ||
        db_exec(ix->db,
            "DELETE FROM art WHERE id NOT IN (SELECT art_id FROM album WHERE art_id IS NOT NULL)"
            " AND id NOT IN (SELECT art_id FROM artist WHERE art_id IS NOT NULL)") != 0)
        goto out;

    stats->changed = sqlite3_total_changes(ix->db) != before;
    if (stats->changed &&
        db_exec(ix->db, "UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = '"
                        LIBRARY_META_GENERATION "'") != 0)
        goto out;

    char ts[64];
    snprintf(ts, sizeof(ts), "UPDATE meta SET value = '%lld' WHERE key = '"
             LIBRARY_META_LAST_SCAN "'", (long long)time(NULL));
    if (db_exec(ix->db, ts) != 0)
        goto out;

    stmts_free(&s);
    if (db_exec(ix->db, "COMMIT") != 0)
        goto out;
    in_txn = false;
    rc = 0;

out:
    stmts_free(&s);
    if (in_txn)
        db_exec(ix->db, "ROLLBACK");
    if (rc != 0) {
        if (stop_requested(ix))
            fprintf(stderr, "indexer: pass interrupted, rolled back\n");
        else
            fprintf(stderr, "indexer: pass failed, rolled back\n");
        stats->changed = false;
    }
    cleanup_art_dir(ix);
    stats->generation = read_generation(ix->db);
    for (size_t k = 0; k < rows.n; k++)
        free(rows.v[k].path);
    free(rows.v);
    pass_reset(ix);
    return rc;
}
