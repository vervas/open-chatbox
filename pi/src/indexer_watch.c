/**
 * indexer_watch.c — Pass scheduling and Linux inotify for chatbox-indexer.
 * See indexer_watch.h.
 */

#include "indexer_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Scheduler                                                               */
/* ---------------------------------------------------------------------- */

void sched_init(indexer_sched_t *s, uint64_t now_ms,
                uint64_t debounce_ms, uint64_t rescan_ms)
{
    memset(s, 0, sizeof(*s));
    s->debounce_ms  = debounce_ms;
    s->rescan_ms    = rescan_ms;
    s->force        = true;
    s->last_pass_ms = now_ms;
}

void sched_event(indexer_sched_t *s, uint64_t now_ms)
{
    s->event_pending = true;
    s->last_event_ms = now_ms;
}

void sched_force(indexer_sched_t *s)
{
    s->force = true;
}

uint64_t sched_wait_ms(const indexer_sched_t *s, uint64_t now_ms)
{
    if (s->force)
        return 0;

    uint64_t due = s->last_pass_ms + s->rescan_ms;
    if (s->event_pending && s->last_event_ms + s->debounce_ms < due)
        due = s->last_event_ms + s->debounce_ms;
    return due > now_ms ? due - now_ms : 0;
}

bool sched_due(const indexer_sched_t *s, uint64_t now_ms)
{
    return sched_wait_ms(s, now_ms) == 0;
}

void sched_pass_done(indexer_sched_t *s, uint64_t now_ms)
{
    /* Single-threaded: events are only noted between passes, so everything
     * pending was covered by this pass. Events that happen during the pass
     * are read from the kernel queue afterwards and start a new debounce. */
    s->force         = false;
    s->event_pending = false;
    s->last_pass_ms  = now_ms;
}

/* ---------------------------------------------------------------------- */
/* inotify watcher                                                         */
/* ---------------------------------------------------------------------- */

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define WATCH_MASK (IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | \
                    IN_DELETE | IN_CREATE)
#define WATCH_MAX_DEPTH 32

struct indexer_watch {
    int    fd;
    char  *root;
    char **paths;      /* indexed by watch descriptor */
    unsigned *visit;   /* per wd: traversal that last visited it */
    int    npaths;
    unsigned traversal;
    bool   warned_full;
};

/* Record `path` for `wd`. Returns false if `wd` was already visited in the
 * current traversal (a directory reached twice: symlink loop, bind mount). */
static bool wd_visit(indexer_watch_t *w, int wd, const char *path)
{
    if (wd >= w->npaths) {
        int n = w->npaths ? w->npaths : 64;
        while (n <= wd)
            n *= 2;
        char **np = realloc(w->paths, (size_t)n * sizeof(*np));
        if (!np)
            return false;
        memset(np + w->npaths, 0, (size_t)(n - w->npaths) * sizeof(*np));
        w->paths = np;
        unsigned *nv = realloc(w->visit, (size_t)n * sizeof(*nv));
        if (!nv)
            return false;
        memset(nv + w->npaths, 0, (size_t)(n - w->npaths) * sizeof(*nv));
        w->visit  = nv;
        w->npaths = n;
    }
    if (w->visit[wd] == w->traversal)
        return false;
    w->visit[wd] = w->traversal;
    free(w->paths[wd]);
    w->paths[wd] = strdup(path);
    return true;
}

/* Watch `path` and every non-hidden directory below it. inotify returns the
 * existing descriptor when an inode is already watched, so a directory
 * reached twice in one traversal (symlink loop, bind mount) is not descended
 * again, while a directory moved inside the tree gets its paths refreshed.
 * Start a traversal with add_tree_root(). */
static void add_tree(indexer_watch_t *w, const char *path, int depth)
{
    if (depth > WATCH_MAX_DEPTH)
        return;

    int wd = inotify_add_watch(w->fd, path, WATCH_MASK | IN_ONLYDIR);
    if (wd < 0) {
        if (errno == ENOSPC && !w->warned_full) {
            fprintf(stderr, "indexer: inotify watch limit reached, relying on rescans "
                    "(raise fs.inotify.max_user_watches)\n");
            w->warned_full = true;
        } else if (errno != ENOSPC && errno != ENOTDIR && errno != ENOENT) {
            fprintf(stderr, "indexer: inotify_add_watch '%s': %s\n", path, strerror(errno));
        }
        return;
    }
    if (!wd_visit(w, wd, path))
        return;

    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name) >= (int)sizeof(sub))
            continue;
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            add_tree(w, sub, depth + 1);
    }
    closedir(d);
}

static void add_tree_root(indexer_watch_t *w, const char *path)
{
    w->traversal++;
    if (w->traversal == 0)
        w->traversal = 1;
    add_tree(w, path, 0);
}

static int watch_start(indexer_watch_t *w)
{
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        fprintf(stderr, "indexer: inotify_init1: %s\n", strerror(errno));
        return -1;
    }
    add_tree_root(w, w->root);
    return 0;
}

static void watch_stop(indexer_watch_t *w)
{
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    for (int i = 0; i < w->npaths; i++) {
        free(w->paths[i]);
        w->paths[i] = NULL;
    }
}

indexer_watch_t *indexer_watch_open(const char *root)
{
    indexer_watch_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->fd   = -1;
    w->root = strdup(root);
    if (!w->root || watch_start(w) != 0) {
        indexer_watch_close(w);
        return NULL;
    }
    return w;
}

int indexer_watch_fd(const indexer_watch_t *w)
{
    return w ? w->fd : -1;
}

int indexer_watch_read(indexer_watch_t *w, bool *overflow)
{
    *overflow = false;
    int relevant = 0;
    char buf[16384] __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        ssize_t len = read(w->fd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EINTR)
                continue;
            break;      /* EAGAIN: drained */
        }
        if (len == 0)
            break;

        for (char *p = buf; p < buf + len;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            p += sizeof(*ev) + ev->len;

            if (ev->mask & IN_Q_OVERFLOW) {
                *overflow = true;
                continue;
            }
            if (ev->mask & IN_IGNORED) {
                if (ev->wd >= 0 && ev->wd < w->npaths) {
                    free(w->paths[ev->wd]);
                    w->paths[ev->wd] = NULL;
                }
                continue;
            }
            if (ev->len == 0 || ev->name[0] == '.')
                continue;   /* hidden files: rsync temps, AppleDouble */

            relevant++;
            if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO)) &&
                ev->wd >= 0 && ev->wd < w->npaths && w->paths[ev->wd]) {
                char sub[PATH_MAX];
                if (snprintf(sub, sizeof(sub), "%s/%s", w->paths[ev->wd],
                             ev->name) < (int)sizeof(sub))
                    add_tree_root(w, sub);
            }
        }
    }

    if (*overflow) {
        /* Events were lost, including directory creations: rebuild every
         * watch. The caller forces a full pass. */
        fprintf(stderr, "indexer: inotify queue overflow, rebuilding watches\n");
        watch_stop(w);
        if (watch_start(w) != 0)
            fprintf(stderr, "indexer: inotify restart failed, relying on rescans\n");
    }
    return relevant;
}

void indexer_watch_close(indexer_watch_t *w)
{
    if (!w)
        return;
    watch_stop(w);
    free(w->paths);
    free(w->visit);
    free(w->root);
    free(w);
}

#else /* !__linux__: poll-only (D8) */

indexer_watch_t *indexer_watch_open(const char *root)
{
    (void)root;
    return NULL;
}

int indexer_watch_fd(const indexer_watch_t *w)
{
    (void)w;
    return -1;
}

int indexer_watch_read(indexer_watch_t *w, bool *overflow)
{
    (void)w;
    *overflow = false;
    return 0;
}

void indexer_watch_close(indexer_watch_t *w)
{
    (void)w;
}

#endif
