/**
 * indexer_probe.h — Tag reader and thumbnailer interface for chatbox-indexer
 * (docs/player-spec.md §3, D7).
 *
 * The scanner only talks to an indexer_probe_ops_t. The real implementation
 * (indexer_probe_ffmpeg) spawns ffprobe/ffmpeg with argv arrays via
 * posix_spawnp — never a shell, since paths contain spaces, quotes and
 * unicode — with a per-spawn timeout. Tests plug in stubs.
 */

#ifndef CHATBOX_INDEXER_PROBE_H
#define CHATBOX_INDEXER_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROBE_STR_LEN 256

/* Tags of one media file. Empty string / 0 / -1 = unknown. */
typedef struct {
    char    title[PROBE_STR_LEN];
    char    artist[PROBE_STR_LEN];
    char    album_artist[PROBE_STR_LEN];
    char    album[PROBE_STR_LEN];
    int     track_no;       /* 0 = unknown                   */
    int     disc_no;        /* 0 = unknown                   */
    int     year;           /* 0 = unknown                   */
    int64_t duration_ms;    /* -1 = unknown                  */
    bool    has_art;        /* an attached picture stream    */
} probe_tags_t;

typedef struct indexer_probe_ops {
    /* Read tags of `path`. 0 = ok, -1 = failed (file is skipped this pass). */
    int (*probe)(void *ctx, const char *path, probe_tags_t *out);

    /* Write the embedded picture of media file `path`, as the original image
     * bytes, to `out_path`. 0 = ok, -1 = no art or failure. */
    int (*extract_art)(void *ctx, const char *path, const char *out_path);

    /* Scale image `src` to a square size×size PNG at `out_path`
     * (cropped to fill). 0 = ok, -1 = failure. */
    int (*thumbnail)(void *ctx, const char *src, int size, const char *out_path);

    void *ctx;
} indexer_probe_ops_t;

/** The real ffprobe/ffmpeg implementation (binaries looked up on PATH). */
const indexer_probe_ops_t *indexer_probe_ffmpeg(void);

/**
 * Parse `ffprobe -print_format json -show_format -show_streams` output.
 * Tags are looked up case-insensitively in format.tags, then in the first
 * audio stream's tags (ogg/opus keep them there). Exposed for tests.
 * @return 0 on success, -1 if the JSON is unusable (no audio stream).
 */
int indexer_probe_parse_json(const char *json, probe_tags_t *out);

/**
 * Run argv[0] (looked up on PATH) with stdin and stderr on /dev/null.
 * If `out` is non-NULL, stdout is captured into a malloc'd, NUL-terminated
 * buffer (*out, freed by the caller; capped at 4 MiB), else discarded.
 * The child is killed after `timeout_ms`.
 * @return the exit status (0 = success), or -1 on spawn failure, timeout or
 *         abnormal termination.
 */
int indexer_run(char *const argv[], char **out, int timeout_ms);

#endif /* CHATBOX_INDEXER_PROBE_H */
