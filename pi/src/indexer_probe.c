/**
 * indexer_probe.c — ffprobe/ffmpeg tag reader and thumbnailer for
 * chatbox-indexer. See indexer_probe.h.
 */

#include "indexer_probe.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define PROBE_TIMEOUT_MS   15000
#define FFMPEG_TIMEOUT_MS  30000
#define RUN_OUT_CAP        (4u * 1024u * 1024u)

/* ---------------------------------------------------------------------- */
/* Child processes                                                         */
/* ---------------------------------------------------------------------- */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Reap `pid`, killing it first if `kill_it`. Returns the exit status or -1. */
static int reap(pid_t pid, bool kill_it)
{
    if (kill_it)
        kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (kill_it)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int indexer_run(char *const argv[], char **out, int timeout_ms)
{
    int pfd[2] = { -1, -1 };
    if (out) {
        *out = NULL;
        if (pipe(pfd) != 0) {
            fprintf(stderr, "indexer: pipe: %s\n", strerror(errno));
            return -1;
        }
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (out) {
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, pfd[0]);
        posix_spawn_file_actions_addclose(&fa, pfd[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }

    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out)
        close(pfd[1]);
    if (rc != 0) {
        fprintf(stderr, "indexer: cannot run %s: %s\n", argv[0], strerror(rc));
        if (out)
            close(pfd[0]);
        return -1;
    }

    int64_t deadline = mono_ms() + timeout_ms;

    if (!out) {
        /* No output to drain: poll for exit until the deadline. */
        for (;;) {
            int st = 0;
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid)
                return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (r < 0 && errno != EINTR)
                return -1;
            if (mono_ms() >= deadline) {
                fprintf(stderr, "indexer: %s timed out, killed\n", argv[0]);
                return reap(pid, true);
            }
            struct timespec nap = { 0, 10 * 1000000L };
            nanosleep(&nap, NULL);
        }
    }

    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    bool failed = (buf == NULL);

    while (!failed) {
        int64_t left = deadline - mono_ms();
        if (left <= 0) {
            fprintf(stderr, "indexer: %s timed out, killed\n", argv[0]);
            failed = true;
            break;
        }
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int n = poll(&p, 1, (int)left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            failed = true;
            break;
        }
        if (n == 0)
            continue;   /* re-check the deadline */
        if (len + 1 >= cap) {
            if (cap >= RUN_OUT_CAP) {
                fprintf(stderr, "indexer: %s output too large\n", argv[0]);
                failed = true;
                break;
            }
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                failed = true;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(pfd[0], buf + len, cap - len - 1);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            failed = true;
            break;
        }
        if (r == 0)
            break;      /* EOF */
        len += (size_t)r;
    }
    close(pfd[0]);

    int status = reap(pid, failed);
    if (failed || status != 0) {
        free(buf);
        return failed ? -1 : status;
    }
    buf[len] = '\0';
    *out = buf;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* ffprobe JSON                                                            */
/* ---------------------------------------------------------------------- */

/* Case-insensitive lookup of the first of `keys` present in a tags object. */
static const char *tag_get(const cJSON *tags, const char *const keys[])
{
    if (!cJSON_IsObject(tags))
        return NULL;
    for (int k = 0; keys[k]; k++) {
        const cJSON *it;
        cJSON_ArrayForEach(it, tags) {
            if (it->string && cJSON_IsString(it) && it->valuestring[0] &&
                strcasecmp(it->string, keys[k]) == 0)
                return it->valuestring;
        }
    }
    return NULL;
}

static const char *tag_any(const cJSON *fmt_tags, const cJSON *st_tags,
                           const char *const keys[])
{
    const char *v = tag_get(fmt_tags, keys);
    return v ? v : tag_get(st_tags, keys);
}

static void copy_tag(char *dst, const char *v)
{
    if (!v) {
        dst[0] = '\0';
        return;
    }
    while (*v == ' ' || *v == '\t')
        v++;
    snprintf(dst, PROBE_STR_LEN, "%s", v);
    size_t n = strlen(dst);
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t' ||
                     dst[n - 1] == '\r' || dst[n - 1] == '\n'))
        dst[--n] = '\0';
}

/* Leading integer of "3", "3/12", "2019-05-01"; 0 if none. */
static int lead_int(const char *v)
{
    if (!v)
        return 0;
    while (*v == ' ')
        v++;
    long n = 0;
    int digits = 0;
    while (*v >= '0' && *v <= '9' && digits < 9) {
        n = n * 10 + (*v - '0');
        v++;
        digits++;
    }
    return (int)n;
}

static int64_t seconds_to_ms(const cJSON *j)
{
    if (!cJSON_IsString(j))
        return -1;
    char *end = NULL;
    double s = strtod(j->valuestring, &end);
    if (end == j->valuestring || s < 0)
        return -1;
    return (int64_t)(s * 1000.0 + 0.5);
}

int indexer_probe_parse_json(const char *json, probe_tags_t *out)
{
    memset(out, 0, sizeof(*out));
    out->duration_ms = -1;

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    const cJSON *streams = cJSON_GetObjectItemCaseSensitive(root, "streams");
    const cJSON *format  = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *audio   = NULL;
    const cJSON *st;
    cJSON_ArrayForEach(st, streams) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(st, "codec_type");
        const cJSON *disp = cJSON_GetObjectItemCaseSensitive(st, "disposition");
        const cJSON *ap   = cJSON_GetObjectItemCaseSensitive(disp, "attached_pic");
        if (!cJSON_IsString(type))
            continue;
        if (strcmp(type->valuestring, "audio") == 0 && !audio)
            audio = st;
        else if (strcmp(type->valuestring, "video") == 0 &&
                 cJSON_IsNumber(ap) && ap->valueint == 1)
            out->has_art = true;
    }
    if (!audio) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *ft = cJSON_GetObjectItemCaseSensitive(format, "tags");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(audio, "tags");

    static const char *const k_title[]  = { "title", NULL };
    static const char *const k_artist[] = { "artist", NULL };
    static const char *const k_aartist[] = { "album_artist", "albumartist",
                                             "album artist", NULL };
    static const char *const k_album[]  = { "album", NULL };
    static const char *const k_track[]  = { "track", "tracknumber", NULL };
    static const char *const k_disc[]   = { "disc", "discnumber", NULL };
    static const char *const k_date[]   = { "date", "year", "originaldate", NULL };

    copy_tag(out->title,        tag_any(ft, at, k_title));
    copy_tag(out->artist,       tag_any(ft, at, k_artist));
    copy_tag(out->album_artist, tag_any(ft, at, k_aartist));
    copy_tag(out->album,        tag_any(ft, at, k_album));
    out->track_no = lead_int(tag_any(ft, at, k_track));
    out->disc_no  = lead_int(tag_any(ft, at, k_disc));
    out->year     = lead_int(tag_any(ft, at, k_date));
    if (out->year < 1000 || out->year > 9999)
        out->year = 0;

    out->duration_ms = seconds_to_ms(cJSON_GetObjectItemCaseSensitive(format, "duration"));
    if (out->duration_ms < 0)
        out->duration_ms = seconds_to_ms(cJSON_GetObjectItemCaseSensitive(audio, "duration"));

    cJSON_Delete(root);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Real implementation                                                     */
/* ---------------------------------------------------------------------- */

/* ffmpeg treats "-" and "proto:..." specially; "file:" forces a plain path. */
static void file_url(char *dst, size_t n, const char *path)
{
    snprintf(dst, n, "file:%s", path);
}

static int ff_probe(void *ctx, const char *path, probe_tags_t *out)
{
    (void)ctx;
    char in[4200];
    file_url(in, sizeof(in), path);
    char *argv[] = { "ffprobe", "-v", "error", "-print_format", "json",
                     "-show_format", "-show_streams", in, NULL };
    char *json = NULL;
    int rc = indexer_run(argv, &json, PROBE_TIMEOUT_MS);
    if (rc != 0) {
        fprintf(stderr, "indexer: ffprobe failed on '%s' (%d)\n", path, rc);
        return -1;
    }
    rc = indexer_probe_parse_json(json, out);
    free(json);
    if (rc != 0)
        fprintf(stderr, "indexer: ffprobe: no audio stream in '%s'\n", path);
    return rc;
}

static int ff_extract_art(void *ctx, const char *path, const char *out_path)
{
    (void)ctx;
    char in[4200], outp[4200];
    file_url(in, sizeof(in), path);
    file_url(outp, sizeof(outp), out_path);
    char *argv[] = { "ffmpeg", "-nostdin", "-v", "error", "-y", "-i", in,
                     "-map", "0:v:0", "-c", "copy", "-frames:v", "1",
                     "-f", "image2", "-update", "1", outp, NULL };
    int rc = indexer_run(argv, NULL, FFMPEG_TIMEOUT_MS);
    return rc == 0 ? 0 : -1;
}

static int ff_thumbnail(void *ctx, const char *src, int size, const char *out_path)
{
    (void)ctx;
    char in[4200], outp[4200], vf[128];
    file_url(in, sizeof(in), src);
    file_url(outp, sizeof(outp), out_path);
    snprintf(vf, sizeof(vf),
             "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d",
             size, size, size, size);
    char *argv[] = { "ffmpeg", "-nostdin", "-v", "error", "-y", "-i", in,
                     "-vf", vf, "-frames:v", "1", "-c:v", "png",
                     "-f", "image2", "-update", "1", outp, NULL };
    int rc = indexer_run(argv, NULL, FFMPEG_TIMEOUT_MS);
    if (rc != 0)
        fprintf(stderr, "indexer: ffmpeg thumbnail %d of '%s' failed (%d)\n",
                size, src, rc);
    return rc == 0 ? 0 : -1;
}

static const indexer_probe_ops_t g_ffmpeg_ops = {
    .probe       = ff_probe,
    .extract_art = ff_extract_art,
    .thumbnail   = ff_thumbnail,
    .ctx         = NULL,
};

const indexer_probe_ops_t *indexer_probe_ffmpeg(void)
{
    return &g_ffmpeg_ops;
}
