/**
 * indexer_main.c — chatbox-indexer daemon entry point
 * (docs/player-spec.md §3, §10).
 *
 *   chatbox-indexer [--config /etc/chatbox.conf] [--music-dir D] [--db F]
 *                   [--art-dir D] [--poll-secs N] [--once]
 *
 * Flags override the config keys (music_dir, music_db, music_art_dir).
 * Without --config the built-in defaults apply. --once runs one pass and
 * exits (0 = the pass committed).
 *
 * Loop: single-threaded, poll() on the inotify fd (Linux) and a signal
 * self-pipe; passes are scheduled by indexer_sched_t (startup pass, 5 s
 * debounce after the last event, a full rescan every --poll-secs, default
 * 30 min, and right away on IN_Q_OVERFLOW). SIGTERM/SIGINT exit cleanly,
 * interrupting a running pass (it rolls back).
 */

#include "config.h"
#include "indexer_probe.h"
#include "indexer_scan.h"
#include "indexer_watch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static int g_sig_pipe[2] = { -1, -1 };

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    int saved = errno;
    if (g_sig_pipe[1] >= 0) {
        ssize_t r = write(g_sig_pipe[1], "x", 1);
        (void)r;
    }
    errno = saved;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--config FILE] [--music-dir DIR] [--db FILE] [--art-dir DIR]\n"
            "          [--poll-secs N] [--once]\n", argv0);
}

static void run_pass(indexer_t *ix)
{
    indexer_stats_t st;
    uint64_t t0 = now_ms();
    int rc = indexer_pass(ix, &st);
    fprintf(stderr, "indexer: pass %s in %llu ms: %d files, %d probed (%d failed), "
            "+%d ~%d -%d, generation %lld%s\n",
            rc == 0 ? "done" : "aborted", (unsigned long long)(now_ms() - t0),
            st.files, st.probed, st.probe_failed, st.added, st.updated, st.removed,
            (long long)st.generation, st.changed ? " (changed)" : "");
}

int main(int argc, char **argv)
{
    const char *config_path = NULL, *music_dir = NULL, *db = NULL, *art_dir = NULL;
    long poll_secs = INDEXER_RESCAN_SECS;
    bool once = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_val = i + 1 < argc;
        if (strcmp(a, "--once") == 0) {
            once = true;
        } else if (strcmp(a, "--config") == 0 && has_val) {
            config_path = argv[++i];
        } else if (strcmp(a, "--music-dir") == 0 && has_val) {
            music_dir = argv[++i];
        } else if (strcmp(a, "--db") == 0 && has_val) {
            db = argv[++i];
        } else if (strcmp(a, "--art-dir") == 0 && has_val) {
            art_dir = argv[++i];
        } else if (strcmp(a, "--poll-secs") == 0 && has_val) {
            char *end = NULL;
            poll_secs = strtol(argv[++i], &end, 10);
            if (!end || *end || poll_secs < 1) {
                fprintf(stderr, "indexer: --poll-secs needs a positive number\n");
                return 2;
            }
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    /* Config: only the music keys matter here. config_parse() also demands
     * homeserver_url/access_token (the real file always has them); when they
     * are missing it returns -1 but has still filled the music keys, so
     * that's only a warning. A file that can't be opened is fatal. */
    static config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.music_dir, sizeof(cfg.music_dir), "%s", CONFIG_DEFAULT_MUSIC_DIR);
    snprintf(cfg.music_db, sizeof(cfg.music_db), "%s", CONFIG_DEFAULT_MUSIC_DB);
    snprintf(cfg.music_art_dir, sizeof(cfg.music_art_dir), "%s", CONFIG_DEFAULT_MUSIC_ART_DIR);
    if (config_path) {
        if (access(config_path, R_OK) != 0) {
            fprintf(stderr, "indexer: cannot read config '%s': %s\n",
                    config_path, strerror(errno));
            return 1;
        }
        if (config_parse(config_path, &cfg) != 0)
            fprintf(stderr, "indexer: config '%s' is incomplete for chat; "
                    "using its music keys anyway\n", config_path);
    }
    if (!music_dir) music_dir = cfg.music_dir;
    if (!db)        db        = cfg.music_db;
    if (!art_dir)   art_dir   = cfg.music_art_dir;

    fprintf(stderr, "indexer: music_dir=%s db=%s art_dir=%s poll=%lds%s\n",
            music_dir, db, art_dir, poll_secs, once ? " (once)" : "");

    if (pipe(g_sig_pipe) == 0) {
        for (int k = 0; k < 2; k++) {
            fcntl(g_sig_pipe[k], F_SETFL, fcntl(g_sig_pipe[k], F_GETFL) | O_NONBLOCK);
            fcntl(g_sig_pipe[k], F_SETFD, FD_CLOEXEC);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    indexer_opts_t opts = {
        .music_dir = music_dir, .db_path = db, .art_dir = art_dir,
        .ops = indexer_probe_ffmpeg(), .stop = &g_stop,
    };
    indexer_t *ix = indexer_open(&opts);
    if (!ix)
        return 1;

    if (once) {
        indexer_stats_t st;
        int rc = indexer_pass(ix, &st);
        fprintf(stderr, "indexer: %d files, %d probed (%d failed), +%d ~%d -%d, "
                "generation %lld%s\n", st.files, st.probed, st.probe_failed,
                st.added, st.updated, st.removed, (long long)st.generation,
                st.changed ? " (changed)" : "");
        indexer_close(ix);
        return rc == 0 ? 0 : 1;
    }

    indexer_watch_t *w = indexer_watch_open(music_dir);
    if (w)
        fprintf(stderr, "indexer: watching %s (inotify)\n", music_dir);
    else
        fprintf(stderr, "indexer: no inotify, rescanning every %ld s\n", poll_secs);

    indexer_sched_t sched;
    sched_init(&sched, now_ms(), INDEXER_DEBOUNCE_MS, (uint64_t)poll_secs * 1000u);

    while (!g_stop) {
        uint64_t now = now_ms();
        if (sched_due(&sched, now)) {
            run_pass(ix);
            sched_pass_done(&sched, now_ms());
            continue;
        }

        uint64_t wait = sched_wait_ms(&sched, now);
        if (wait > 60000)
            wait = 60000;   /* re-evaluate at least once a minute */
        struct pollfd pfd[2] = {
            { .fd = g_sig_pipe[0], .events = POLLIN },
            { .fd = indexer_watch_fd(w), .events = POLLIN },
        };
        int n = poll(pfd, 2, (int)wait);
        if (n < 0 && errno != EINTR) {
            fprintf(stderr, "indexer: poll: %s\n", strerror(errno));
            break;
        }
        if (n > 0 && (pfd[1].revents & POLLIN)) {
            bool overflow = false;
            int ev = indexer_watch_read(w, &overflow);
            if (overflow)
                sched_force(&sched);
            else if (ev > 0)
                sched_event(&sched, now_ms());
        }
    }

    fprintf(stderr, "indexer: exiting\n");
    indexer_watch_close(w);
    indexer_close(ix);
    return 0;
}
