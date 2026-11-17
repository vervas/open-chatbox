/**
 * indexer_scan.h — One indexing pass of chatbox-indexer
 * (docs/player-spec.md §1–§3). Schema in library_schema.h.
 *
 * A pass:
 *   1. walks music_dir (hidden files, "._*" AppleDouble files, partial
 *      files (*.part.*, *.temp.*) and unsupported extensions are ignored; symlinks are
 *      followed but a directory is never entered twice, so loops can't hang);
 *   2. compares mtime+size with the track rows and probes only new or
 *      changed files (a file that fails to probe is skipped and logged: a new
 *      one is not added, a changed one keeps its old row, and both are
 *      retried next pass);
 *   3. in ONE transaction: deletes rows of vanished files, writes the probed
 *      ones, removes albums/artists left empty, resolves album art
 *      (cover|folder|front.(jpg|jpeg|png) → embedded → NULL) and artist art
 *      (newest album with art), drops unreferenced art rows, and bumps
 *      meta.generation if anything changed;
 *   4. after the transaction, deletes thumbnails no row references.
 *
 * Thumbnails for new art are written to temp names and renamed into place
 * before the commit that references them.
 *
 * Album identity is the directory (album.dir); see library_schema.h for the
 * _inbox exception. Album title/artist/year come from the first track
 * (in path order) probed in the pass, tags first, then the path:
 *   Artist/Album/NN Title.ext; _inbox/ with no artist tag → "Unknown";
 *   Compilations/ → "Various Artists"; album artist = album_artist tag,
 *   else the artist tag.
 *
 * Not thread-safe; one indexer_t per DB writer.
 */

#ifndef CHATBOX_INDEXER_SCAN_H
#define CHATBOX_INDEXER_SCAN_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "indexer_probe.h"

typedef struct {
    const char *music_dir;
    const char *db_path;
    const char *art_dir;
    const indexer_probe_ops_t *ops;
    volatile sig_atomic_t *stop;    /* optional: abort a pass when set */
} indexer_opts_t;

typedef struct {
    int     files;          /* media files seen                          */
    int     probed;         /* probe calls made                          */
    int     probe_failed;   /* files skipped because the probe failed    */
    int     added;          /* new track rows                            */
    int     updated;        /* re-probed track rows                      */
    int     removed;        /* deleted track rows                        */
    bool    changed;        /* the pass changed the DB (generation bump) */
    int64_t generation;     /* meta.generation after the pass            */
} indexer_stats_t;

typedef struct indexer indexer_t;

/**
 * Open (creating if needed) the DB: WAL mode, schema, user_version and
 * meta.schema_version, busy_timeout. Creates art_dir.
 * @return NULL on error (logged).
 */
indexer_t *indexer_open(const indexer_opts_t *opts);

/**
 * Run one pass. Never fatal for bad files.
 * @return 0 when the pass committed (or had nothing to do), -1 when it was
 *         aborted and rolled back (music_dir missing, DB error, stop flag).
 */
int indexer_pass(indexer_t *ix, indexer_stats_t *stats);

void indexer_close(indexer_t *ix);

/* Helpers, exposed for tests. */

/** Case-folded (ASCII) name with a leading "The " dropped. */
void indexer_sort_name(const char *name, char *out, size_t out_size);

/** True for a supported, non-hidden, non-partial media file name. */
bool indexer_is_media_name(const char *name);

/** Cover file rank: 0 cover, 1 folder, 2 front (then jpg<jpeg<png), -1 none.
 *  Lower is better. Case-insensitive. */
int indexer_cover_rank(const char *name);

/** Lowercase hex SHA-1 (41 bytes incl. NUL) of a file. 0 ok, -1 error. */
int indexer_sha1_file(const char *path, char out[41]);

#endif /* CHATBOX_INDEXER_SCAN_H */
