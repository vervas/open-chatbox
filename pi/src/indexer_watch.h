/**
 * indexer_watch.h — When chatbox-indexer runs a pass (docs/player-spec.md §3).
 *
 * Two parts:
 *
 *  1. indexer_sched_t — pure pass scheduling on a caller-supplied
 *     millisecond clock (so tests drive it with a fake clock):
 *       - a pass is due right away at startup and after an inotify queue
 *         overflow (sched_force);
 *       - file-system events are debounced: a pass is due `debounce_ms`
 *         after the LAST event (events at 0..4 s → one pass at 9 s);
 *       - a full rescan is due `rescan_ms` after the previous pass,
 *         whatever happened (the safety net; the only trigger off Linux).
 *
 *  2. indexer_watch_t — Linux inotify on the whole music tree (D8):
 *     IN_CLOSE_WRITE, IN_MOVED_*, IN_DELETE, IN_CREATE, adding watches for
 *     new directories. Elsewhere indexer_watch_open() returns NULL and the
 *     daemon polls on the rescan timer only.
 */

#ifndef CHATBOX_INDEXER_WATCH_H
#define CHATBOX_INDEXER_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#define INDEXER_DEBOUNCE_MS   5000u
#define INDEXER_RESCAN_SECS   (30u * 60u)

/* ---------------------------------------------------------------------- */
/* Scheduler                                                               */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint64_t debounce_ms;
    uint64_t rescan_ms;
    bool     force;           /* pass due now (startup / overflow)     */
    bool     event_pending;   /* debounced events waiting              */
    uint64_t last_event_ms;
    uint64_t last_pass_ms;
} indexer_sched_t;

/** Start the schedule at `now_ms`, with the startup pass due immediately. */
void sched_init(indexer_sched_t *s, uint64_t now_ms,
                uint64_t debounce_ms, uint64_t rescan_ms);

/** A relevant file-system event happened at `now_ms` (restarts the debounce). */
void sched_event(indexer_sched_t *s, uint64_t now_ms);

/** Make a pass due right away (IN_Q_OVERFLOW). */
void sched_force(indexer_sched_t *s);

/** True when a pass should run at `now_ms`. */
bool sched_due(const indexer_sched_t *s, uint64_t now_ms);

/** Milliseconds until the next pass is due (0 = due now), for poll(). */
uint64_t sched_wait_ms(const indexer_sched_t *s, uint64_t now_ms);

/** A pass finished at `now_ms`: clears pending events and the force flag. */
void sched_pass_done(indexer_sched_t *s, uint64_t now_ms);

/* ---------------------------------------------------------------------- */
/* inotify watcher (Linux only)                                            */
/* ---------------------------------------------------------------------- */

typedef struct indexer_watch indexer_watch_t;

/** Watch every (non-hidden) directory under `root`. NULL off Linux or on error. */
indexer_watch_t *indexer_watch_open(const char *root);

/** The fd to poll() for POLLIN. */
int indexer_watch_fd(const indexer_watch_t *w);

/**
 * Drain pending events without blocking. Adds watches for new directories.
 * @param overflow  set true on IN_Q_OVERFLOW (watches are rebuilt; the
 *                  caller should force a pass)
 * @return number of relevant events (0 = nothing that needs a pass)
 */
int indexer_watch_read(indexer_watch_t *w, bool *overflow);

void indexer_watch_close(indexer_watch_t *w);

#endif /* CHATBOX_INDEXER_WATCH_H */
