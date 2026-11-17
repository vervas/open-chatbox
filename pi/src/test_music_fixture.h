/**
 * test_music_fixture.h — Shared helpers for the music-library tests
 * (test_indexer, test_library): assertion macros, the stub prober and
 * thumbnailer, and a scratch music tree under $TMPDIR.
 *
 * The prober and thumbnailer are stubs: a "media file" is a text file of
 * key=value lines that the stub probe reads as tags:
 *
 *   title= artist= album_artist= album= track= disc= year= dur=
 *   art=<bytes>   embedded picture (the stub extractor writes <bytes>)
 *   fail=1        the probe fails
 *
 * Cover images are text files too; the stub thumbnailer writes
 * "PNG<size>:<source bytes>" (a source containing "BADIMG" fails).
 * The stubs count their calls in g_stub.
 *
 * Test-only code: not linked into any firmware binary.
 */

#ifndef CHATBOX_TEST_MUSIC_FIXTURE_H
#define CHATBOX_TEST_MUSIC_FIXTURE_H

#include "indexer_probe.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- assertions (counters defined in test_music_fixture.c) ---- */

extern int g_tests;
extern int g_failures;

#define EXPECT(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_INT(got, want, msg) do { \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    g_tests++; \
    if (g_ != w_) { \
        fprintf(stderr, "FAIL [%s:%d] %s: got %lld, want %lld\n", \
                __FILE__, __LINE__, msg, g_, w_); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_STR(got, want, msg) do { \
    const char *g_ = (got), *w_ = (want); \
    g_tests++; \
    if (!g_ || strcmp(g_, w_) != 0) { \
        fprintf(stderr, "FAIL [%s:%d] %s: got '%s', want '%s'\n", \
                __FILE__, __LINE__, msg, g_ ? g_ : "(null)", w_); \
        g_failures++; \
    } \
} while (0)

/* ---- stubs ---- */

typedef struct {
    int  probe_calls;
    int  extract_calls;
    int  thumb_calls;
    char last_probed[PATH_MAX];
} stub_t;

extern stub_t g_stub;
extern const indexer_probe_ops_t g_stub_ops;

/** Whole file into a malloc'd NUL-terminated buffer (caller frees), or NULL. */
char *slurp(const char *path, size_t *len);

/** Write `len` bytes to `path`. 0 ok, -1 error. */
int spit(const char *path, const char *data, size_t len);

/* ---- scratch tree ---- */

extern char g_root[PATH_MAX];   /* $TMPDIR/<name>.XXXXXX            */
extern char g_music[PATH_MAX];  /* <root>/music dir (with a space)  */
extern char g_art[PATH_MAX];    /* <root>/state/art                 */
extern char g_db[PATH_MAX];     /* <root>/state/library.db          */

/** Create a fresh root with music/ and state/ (not art/, not the DB) and
 *  reset the stub counters and the mtime clock. Exits on failure. */
void fixture_make_dirs(const char *name);

/** Remove the whole root. */
void fixture_remove(void);

/** mkdir -p of every parent directory of `path`. */
void mkdirs_for(const char *path);

/** <music>/<rel> */
void abs_path(char *out, size_t n, const char *rel);

/** Write a file under the music dir, with an mtime that strictly increases
 *  per call, so "changed" never depends on clock granularity. */
void put(const char *rel, const char *content);

/** Delete / rename a file under the music dir. */
void rm(const char *rel);
void mv(const char *from, const char *to);

/** rm -rf */
void rm_tree(const char *path);

#endif /* CHATBOX_TEST_MUSIC_FIXTURE_H */
