/**
 * test_music_fixture.c — Shared helpers for test_indexer and test_library.
 * See test_music_fixture.h.
 */

#define _GNU_SOURCE     /* nftw() on glibc */

#include "test_music_fixture.h"

#include <errno.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int g_tests = 0;
int g_failures = 0;

/* ---------------------------------------------------------------------- */
/* Stubs                                                                   */
/* ---------------------------------------------------------------------- */

stub_t g_stub;

char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t r = fread(buf, 1, (size_t)n, f);
    buf[r] = '\0';
    fclose(f);
    if (len)
        *len = r;
    return buf;
}

int spit(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

/* Value of "key=" in a key=value text, or NULL. Writes into out. */
static const char *kv(const char *text, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = ll - kl - 1;
            if (vl >= n)
                vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = '\0';
            return out;
        }
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

static int stub_probe(void *ctx, const char *path, probe_tags_t *t)
{
    (void)ctx;
    g_stub.probe_calls++;
    snprintf(g_stub.last_probed, sizeof(g_stub.last_probed), "%s", path);
    char *text = slurp(path, NULL);
    if (!text)
        return -1;
    memset(t, 0, sizeof(*t));
    t->duration_ms = -1;
    char v[PROBE_STR_LEN];
    int rc = 0;
    if (kv(text, "fail", v, sizeof(v)))
        rc = -1;
    if (kv(text, "title", v, sizeof(v)))        snprintf(t->title, sizeof(t->title), "%s", v);
    if (kv(text, "artist", v, sizeof(v)))       snprintf(t->artist, sizeof(t->artist), "%s", v);
    if (kv(text, "album_artist", v, sizeof(v))) snprintf(t->album_artist, sizeof(t->album_artist), "%s", v);
    if (kv(text, "album", v, sizeof(v)))        snprintf(t->album, sizeof(t->album), "%s", v);
    if (kv(text, "track", v, sizeof(v)))        t->track_no = atoi(v);
    if (kv(text, "disc", v, sizeof(v)))         t->disc_no = atoi(v);
    if (kv(text, "year", v, sizeof(v)))         t->year = atoi(v);
    if (kv(text, "dur", v, sizeof(v)))          t->duration_ms = atoll(v);
    if (kv(text, "art", v, sizeof(v)))          t->has_art = true;
    free(text);
    return rc;
}

static int stub_extract(void *ctx, const char *path, const char *out_path)
{
    (void)ctx;
    g_stub.extract_calls++;
    char *text = slurp(path, NULL);
    if (!text)
        return -1;
    char v[PROBE_STR_LEN];
    int rc = -1;
    if (kv(text, "art", v, sizeof(v)))
        rc = spit(out_path, v, strlen(v));
    free(text);
    return rc;
}

static int stub_thumb(void *ctx, const char *src, int size, const char *out_path)
{
    (void)ctx;
    g_stub.thumb_calls++;
    size_t n;
    char *data = slurp(src, &n);
    if (!data)
        return -1;
    int rc = -1;
    if (strstr(data, "BADIMG") == NULL) {
        char buf[1024];
        int l = snprintf(buf, sizeof(buf), "PNG%d:%s", size, data);
        rc = spit(out_path, buf, (size_t)l);
    }
    free(data);
    return rc;
}

const indexer_probe_ops_t g_stub_ops = {
    .probe = stub_probe, .extract_art = stub_extract, .thumbnail = stub_thumb,
};

/* ---------------------------------------------------------------------- */
/* Scratch tree                                                            */
/* ---------------------------------------------------------------------- */

char g_root[PATH_MAX];
char g_music[PATH_MAX];
char g_art[PATH_MAX];
char g_db[PATH_MAX];

void mkdirs_for(const char *path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
}

void abs_path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/%s", g_music, rel);
}

/* Write a file under the music dir with an explicit mtime offset (seconds
 * from a fixed base) so "changed" never depends on clock granularity. */
static int g_mtime_tick = 0;

void put(const char *rel, const char *content)
{
    char p[PATH_MAX];
    abs_path(p, sizeof(p), rel);
    mkdirs_for(p);
    spit(p, content, strlen(content));
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = 1700000000 + (++g_mtime_tick) * 10;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(p, tv);
}

void rm(const char *rel)
{
    char p[PATH_MAX];
    abs_path(p, sizeof(p), rel);
    unlink(p);
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(p);
}

void rm_tree(const char *path)
{
    nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

void mv(const char *from, const char *to)
{
    char a[PATH_MAX], b[PATH_MAX];
    abs_path(a, sizeof(a), from);
    abs_path(b, sizeof(b), to);
    mkdirs_for(b);
    if (rename(a, b) != 0)
        fprintf(stderr, "test: rename %s: %s\n", from, strerror(errno));
}

void fixture_make_dirs(const char *name)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(g_root, sizeof(g_root), "%s/%s.XXXXXX", tmp, name);
    /* collapse a double slash from TMPDIR ending in '/' */
    char *dd = strstr(g_root, "//");
    if (dd)
        memmove(dd, dd + 1, strlen(dd));
    if (!mkdtemp(g_root)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(g_music, sizeof(g_music), "%s/music dir", g_root);   /* a space */
    snprintf(g_art, sizeof(g_art), "%s/state/art", g_root);
    snprintf(g_db, sizeof(g_db), "%s/state/library.db", g_root);
    mkdir(g_music, 0755);
    char state[PATH_MAX];
    snprintf(state, sizeof(state), "%s/state", g_root);
    mkdir(state, 0755);
    memset(&g_stub, 0, sizeof(g_stub));
    g_mtime_tick = 0;
}

void fixture_remove(void)
{
    rm_tree(g_root);
}
