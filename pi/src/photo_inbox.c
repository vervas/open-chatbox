/**
 * photo_inbox.c — Chat photos into the photo folder (see photo_inbox.h).
 */

#include "photo_inbox.h"
#include "photo_decode.h"
#include "matrix_api.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Backoff after the 1st, 2nd... transient failure. */
static const uint32_t k_backoff_ms[PHOTO_INBOX_TRIES] = { 2000, 8000, 20000, 40000, 60000 };

typedef struct {
    bool           used;
    photo_event_t  ev;
    photo_status_t st;
    int            tries;         /* transient failures, capped at PHOTO_INBOX_TRIES */
    bool           late;          /* PHOTO_INBOX_TRIES failures: shown as FAILED, still retried */
    uint32_t       next_ms;       /* with `waiting`: not before this tick */
    bool           waiting;       /* backing off */
    bool           legacy;        /* use the unauthenticated media endpoint */
    bool           in_flight;
    uint32_t       serial;        /* matches a download callback to its entry */
    uint32_t       order;         /* offer order, for eviction */
    char           name[PHOTO_INBOX_NAME_MAX];
} entry_t;

static char             g_dir[512];
static bool             g_dir_ok;
static photo_inbox_cb_t g_cb;
static void            *g_cb_ud;
static entry_t          g_q[PHOTO_INBOX_QUEUE];
static uint32_t         g_serial;
static uint32_t         g_order;
static uint32_t         g_now;
static bool             g_busy;          /* a download is in flight */

static uint64_t g_seen[PHOTO_INBOX_SEEN_MAX];   /* oldest first */
static int      g_seen_n;

static void real_fetch(const char *mxc, bool legacy, size_t max_bytes,
                       matrix_media_cb_t cb, void *ud)
{
    matrix_async_get_media(mxc, legacy, max_bytes, cb, ud);
}

typedef void (*fetch_fn_t)(const char *, bool, size_t, matrix_media_cb_t, void *);
static fetch_fn_t g_fetch = real_fetch;

/* ---- Small helpers ------------------------------------------------------- */

uint64_t photo_inbox_hash(const char *s)
{
    uint64_t h = 14695981039346656037ull;          /* FNV-1a 64 */
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ull;
    }
    return h;
}

void photo_inbox_filename(const char *event_id, int64_t ts_ms, const char *ext,
                          char out[PHOTO_INBOX_NAME_MAX])
{
    time_t t = (time_t)(ts_ms > 0 ? ts_ms / 1000 : 0);
    struct tm tm;
    gmtime_r(&t, &tm);
    const char *e = (ext && strcmp(ext, "png") == 0) ? "png" : "jpg";
    snprintf(out, PHOTO_INBOX_NAME_MAX, "%04d%02d%02d-%02d%02d%02d-%016llx.%s",
             (tm.tm_year + 1900) % 10000, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (unsigned long long)photo_inbox_hash(event_id), e);
}

static const char *json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static bool json_num(const cJSON *o, const char *k, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

bool photo_inbox_parse(const cJSON *event, photo_event_t *out)
{
    if (!event || !out || !cJSON_IsObject(event)) return false;
    memset(out, 0, sizeof(*out));
    out->size = -1;
    const char *type = json_str(event, "type");
    const char *eid  = json_str(event, "event_id");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(event, "content");
    if (!type || strcmp(type, "m.room.message") != 0 || !eid || !eid[0] ||
        !cJSON_IsObject(content))
        return false;
    const char *mt  = json_str(content, "msgtype");
    const char *url = json_str(content, "url");
    if (!mt || strcmp(mt, "m.image") != 0 || !url || strncmp(url, "mxc://", 6) != 0)
        return false;
    if (strlen(eid) >= sizeof(out->event_id) || strlen(url) >= sizeof(out->mxc))
        return false;
    snprintf(out->event_id, sizeof(out->event_id), "%s", eid);
    snprintf(out->mxc, sizeof(out->mxc), "%s", url);
    double d;
    if (json_num(event, "origin_server_ts", &d) && d > 0 && d < 1e15) out->ts = (int64_t)d;
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(content, "info");
    if (cJSON_IsObject(info)) {
        const char *m = json_str(info, "mimetype");
        if (m) snprintf(out->mimetype, sizeof(out->mimetype), "%s", m);
        if (json_num(info, "size", &d) && d >= 0 && d < 1e15) out->size = (int64_t)d;
        if (json_num(info, "w", &d) && d > 0 && d < 1e6) out->w = (int)d;
        if (json_num(info, "h", &d) && d > 0 && d < 1e6) out->h = (int)d;
    }
    return true;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The saved file of (event_id, ts), if there is one. */
static bool find_saved(const char *event_id, int64_t ts, char *path, size_t n)
{
    static const char *exts[] = { "jpg", "png" };
    for (int i = 0; i < 2; i++) {
        char name[PHOTO_INBOX_NAME_MAX];
        photo_inbox_filename(event_id, ts, exts[i], name);
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", g_dir, name);
        if (file_exists(p)) {
            if (path && n) snprintf(path, n, "%s", p);
            return true;
        }
    }
    return false;
}

/* ---- Seen list ----------------------------------------------------------- */

static void seen_path(char *out, size_t n)
{
    snprintf(out, n, "%s/.chatbox-seen", g_dir);
}

static bool seen_has(uint64_t h)
{
    for (int i = g_seen_n - 1; i >= 0; i--)
        if (g_seen[i] == h) return true;
    return false;
}

bool photo_inbox_seen(const char *event_id)
{
    return seen_has(photo_inbox_hash(event_id));
}

static void seen_load(void)
{
    g_seen_n = 0;
    char p[1024];
    seen_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char *end = NULL;
        unsigned long long v = strtoull(line, &end, 16);
        if (end == line) continue;
        if (g_seen_n == PHOTO_INBOX_SEEN_MAX) {
            memmove(g_seen, g_seen + 1, sizeof(g_seen[0]) * (PHOTO_INBOX_SEEN_MAX - 1));
            g_seen_n--;
        }
        g_seen[g_seen_n++] = (uint64_t)v;
    }
    fclose(f);
}

/* Rewrite the file with what's in memory (temp + rename). */
static void seen_rewrite(void)
{
    char p[1024], tmp[1100];
    seen_path(p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_seen_n; i++) fprintf(f, "%016llx\n", (unsigned long long)g_seen[i]);
    if (fclose(f) == 0) rename(tmp, p);
    else unlink(tmp);
}

static void seen_add(const char *event_id)
{
    uint64_t h = photo_inbox_hash(event_id);
    if (seen_has(h)) return;
    if (g_seen_n == PHOTO_INBOX_SEEN_MAX) {
        /* Keep the newest half. */
        int keep = PHOTO_INBOX_SEEN_MAX / 2;
        memmove(g_seen, g_seen + (g_seen_n - keep), sizeof(g_seen[0]) * (size_t)keep);
        g_seen_n = keep;
        g_seen[g_seen_n++] = h;
        if (g_dir_ok) seen_rewrite();
        return;
    }
    g_seen[g_seen_n++] = h;
    if (!g_dir_ok) return;
    char p[1024];
    seen_path(p, sizeof(p));
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "%016llx\n", (unsigned long long)h);
    fclose(f);
}

/* ---- Saving -------------------------------------------------------------- */

photo_status_t photo_inbox_save(const char *event_id, int64_t ts_ms,
                                const unsigned char *data, size_t len,
                                char *path, size_t n)
{
    if (!event_id || !data || len == 0 || len > PHOTO_INBOX_MAX_BYTES) return PHOTO_ST_REFUSED;
    photo_fmt_t fmt = photo_format_sniff(data, len);
    if (fmt == PHOTO_FMT_NONE) return PHOTO_ST_REFUSED;

    char name[PHOTO_INBOX_NAME_MAX];
    photo_inbox_filename(event_id, ts_ms, fmt == PHOTO_FMT_PNG ? "png" : "jpg", name);
    char final[1024], tmp[1024];
    snprintf(final, sizeof(final), "%s/%s", g_dir, name);
    snprintf(tmp, sizeof(tmp), "%s/.incoming-%016llx.tmp", g_dir,
             (unsigned long long)photo_inbox_hash(event_id));

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        fprintf(stderr, "photos: cannot write in %s: %s\n", g_dir, strerror(errno));
        return PHOTO_ST_FAILED;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)w;
    }
    bool ok = off == len && fsync(fd) == 0;
    if (ok && ts_ms > 0) {
        struct timeval tv[2];
        tv[0].tv_sec = tv[1].tv_sec = (time_t)(ts_ms / 1000);
        tv[0].tv_usec = tv[1].tv_usec = (suseconds_t)((ts_ms % 1000) * 1000);
        futimes(fd, tv);
    }
    if (close(fd) != 0) ok = false;
    if (!ok || rename(tmp, final) != 0) {
        fprintf(stderr, "photos: saving %s failed: %s\n", name, strerror(errno));
        unlink(tmp);
        return PHOTO_ST_FAILED;
    }
    fprintf(stderr, "photos: saved %s (%zu bytes)\n", name, len);
    if (path && n) snprintf(path, n, "%s", final);
    return PHOTO_ST_READY;
}

/* ---- Queue --------------------------------------------------------------- */

static entry_t *find_entry(const char *event_id)
{
    for (int i = 0; i < PHOTO_INBOX_QUEUE; i++)
        if (g_q[i].used && strcmp(g_q[i].ev.event_id, event_id) == 0) return &g_q[i];
    return NULL;
}

static entry_t *find_serial(uint32_t serial)
{
    for (int i = 0; i < PHOTO_INBOX_QUEUE; i++)
        if (g_q[i].used && g_q[i].serial == serial) return &g_q[i];
    return NULL;
}

/* A free slot, or the oldest finished one. NULL: every slot is waiting. */
static entry_t *alloc_entry(void)
{
    entry_t *old = NULL;
    for (int i = 0; i < PHOTO_INBOX_QUEUE; i++) {
        entry_t *e = &g_q[i];
        if (!e->used) return e;
        if (e->st == PHOTO_ST_LOADING) continue;
        if (!old || e->order < old->order) old = e;
    }
    return old;
}

static void finish(entry_t *e, photo_status_t st)
{
    e->st = st;
    e->in_flight = false;
    if (st == PHOTO_ST_READY || st == PHOTO_ST_REFUSED) seen_add(e->ev.event_id);
    if (g_cb) g_cb(e->ev.event_id, st, g_cb_ud);
}

void photo_inbox_offer(const cJSON *event)
{
    photo_event_t ev;
    if (!photo_inbox_parse(event, &ev)) return;
    if (find_entry(ev.event_id)) return;
    if (photo_inbox_seen(ev.event_id)) return;
    if (g_dir_ok && find_saved(ev.event_id, ev.ts, NULL, 0)) {
        seen_add(ev.event_id);              /* saved before the list knew */
        return;
    }
    /* What the event says about itself can already rule it out. */
    bool refuse = false;
    if (ev.mimetype[0] && strcmp(ev.mimetype, "image/jpeg") != 0 &&
        strcmp(ev.mimetype, "image/png") != 0 && strcmp(ev.mimetype, "image/jpg") != 0)
        refuse = true;
    if (ev.size > (int64_t)PHOTO_INBOX_MAX_BYTES) refuse = true;

    entry_t *e = alloc_entry();
    if (!e) {
        fprintf(stderr, "photos: inbox full, %s waits for the next boot\n", ev.event_id);
        return;
    }
    memset(e, 0, sizeof(*e));
    e->used   = true;
    e->ev     = ev;
    e->order  = ++g_order;
    e->serial = ++g_serial;
    if (refuse) {
        fprintf(stderr, "photos: skipping %s (%s, %lld bytes)\n", ev.event_id,
                ev.mimetype[0] ? ev.mimetype : "?", (long long)ev.size);
        finish(e, PHOTO_ST_REFUSED);
        return;
    }
    e->st = PHOTO_ST_LOADING;
    e->waiting = false;
}

/* A download's HTTP failure that no retry can mend: the media isn't there
 * (404 on both endpoints, 410), the request is malformed (400), it's too
 * big (413)... The photo is refused and goes into the seen list. Not:
 * 401 / 403 (the token or the server's set-up, which a grown-up can fix),
 * 408 / 429 (try later): those retry like a network failure and are never
 * written to the seen list. */
static bool http_permanent(long http)
{
    if (http < 400 || http >= 500) return false;
    return http != 401 && http != 403 && http != 408 && http != 429;
}

static void media_done(const unsigned char *data, size_t len, long http, int status, void *ud)
{
    entry_t *e = find_serial((uint32_t)(uintptr_t)ud);
    g_busy = false;
    if (!e || e->st != PHOTO_ST_LOADING) return;
    e->in_flight = false;

    switch (status) {
    case MATRIX_OK: {
        photo_status_t st = photo_inbox_save(e->ev.event_id, e->ev.ts, data, len, NULL, 0);
        if (st == PHOTO_ST_REFUSED) {
            fprintf(stderr, "photos: %s is not a JPEG or PNG (or too big), skipped\n",
                    e->ev.event_id);
            finish(e, st);
            return;
        }
        if (st == PHOTO_ST_READY) {
            finish(e, st);
            return;
        }
        break;          /* disk trouble: retry like a transient failure */
    }
    case MATRIX_ERR_CANCELLED:
        return;         /* shutting down */
    case MATRIX_ERR_QUEUE:
        e->next_ms = g_now + 500;       /* the dispatcher is busy: soon */
        e->waiting = true;
        return;
    case MATRIX_ERR_TOO_LARGE:
    case MATRIX_ERR_REJECTED:
        finish(e, PHOTO_ST_REFUSED);
        return;
    case MATRIX_ERR_HTTP:
        if (!e->legacy && (http == 404 || http == 400 || http == 405)) {
            /* No authenticated media on this server: the old endpoint. */
            e->legacy = true;
            e->waiting = false;
            return;
        }
        if (http_permanent(http)) {
            finish(e, PHOTO_ST_REFUSED);
            return;
        }
        break;
    default:
        break;
    }

    /* Transient: back off and retry for as long as it takes, like a text in
     * the send outbox (matrix_api.c): 2, 8, 20, 40 s, then every 60 s. After
     * PHOTO_INBOX_TRIES the bubble says it didn't arrive (told once); it
     * keeps trying, and a later success shows the picture after all. */
    if (e->tries < PHOTO_INBOX_TRIES) e->tries++;
    if (e->tries >= PHOTO_INBOX_TRIES && !e->late) {
        fprintf(stderr, "photos: %s failed %d times, still trying every %u s\n",
                e->ev.event_id, e->tries, k_backoff_ms[PHOTO_INBOX_TRIES - 1] / 1000);
        e->late = true;
        if (g_cb) g_cb(e->ev.event_id, PHOTO_ST_FAILED, g_cb_ud);
    }
    e->next_ms = g_now + k_backoff_ms[e->tries - 1];
    e->waiting = true;
}

void photo_inbox_tick(uint32_t now_ms)
{
    g_now = now_ms;
    if (g_busy) return;
    entry_t *pick = NULL;
    for (int i = 0; i < PHOTO_INBOX_QUEUE; i++) {
        entry_t *e = &g_q[i];
        if (!e->used || e->st != PHOTO_ST_LOADING) continue;
        if (e->waiting && (int32_t)(now_ms - e->next_ms) < 0) continue;
        if (!pick || e->order < pick->order) pick = e;
    }
    if (!pick) return;
    if (!g_dir_ok) {
        /* Try to make the folder again; nothing to save into otherwise. */
        g_dir_ok = mkdir(g_dir, 02775) == 0 || errno == EEXIST;
        if (!g_dir_ok) {
            pick->tries = PHOTO_INBOX_TRIES - 1;
            media_done(NULL, 0, 0, MATRIX_ERR_TRANSPORT, (void *)(uintptr_t)pick->serial);
            return;
        }
    }
    pick->in_flight = true;
    g_busy = true;
    g_fetch(pick->ev.mxc, pick->legacy, PHOTO_INBOX_MAX_BYTES, media_done,
            (void *)(uintptr_t)pick->serial);
}

photo_status_t photo_inbox_status(const char *event_id, int64_t ts_ms, char *path, size_t n)
{
    if (!event_id) return PHOTO_ST_UNKNOWN;
    entry_t *e = find_entry(event_id);
    if (e && e->st == PHOTO_ST_LOADING) return e->late ? PHOTO_ST_FAILED : PHOTO_ST_LOADING;
    if (find_saved(event_id, ts_ms, path, n)) return PHOTO_ST_READY;
    if (e) return e->st == PHOTO_ST_READY ? PHOTO_ST_GONE : e->st;
    if (photo_inbox_seen(event_id)) return PHOTO_ST_GONE;
    return PHOTO_ST_UNKNOWN;
}

/* ---- Lifecycle ----------------------------------------------------------- */

void photo_inbox_init(const char *photos_dir, photo_inbox_cb_t cb, void *ud)
{
    memset(g_q, 0, sizeof(g_q));
    g_busy = false;
    g_cb = cb;
    g_cb_ud = ud;
    snprintf(g_dir, sizeof(g_dir), "%s", photos_dir ? photos_dir : "");
    size_t len = strlen(g_dir);
    while (len > 1 && g_dir[len - 1] == '/') g_dir[--len] = '\0';
    struct stat st;
    g_dir_ok = stat(g_dir, &st) == 0 && S_ISDIR(st.st_mode);
    if (!g_dir_ok && g_dir[0]) {
        g_dir_ok = mkdir(g_dir, 02775) == 0;
        if (!g_dir_ok)
            fprintf(stderr, "photos: no folder %s (%s): chat photos are not saved\n",
                    g_dir, strerror(errno));
    }
    seen_load();
}

void photo_inbox_deinit(void)
{
    memset(g_q, 0, sizeof(g_q));
    g_busy = false;
    g_cb = NULL;
    g_seen_n = 0;
}

#ifdef CHATBOX_TESTING
void photo_inbox_test_set_fetch(photo_inbox_fetch_fn fn)
{
    g_fetch = fn ? (fetch_fn_t)fn : real_fetch;
}

int photo_inbox_test_pending(void)
{
    int n = 0;
    for (int i = 0; i < PHOTO_INBOX_QUEUE; i++)
        if (g_q[i].used && g_q[i].st == PHOTO_ST_LOADING) n++;
    return n;
}
#endif
