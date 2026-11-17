/**
 * player.h — Music player core: the queue, and an mpv child process driven
 * over its JSON IPC socket (docs/player-spec.md §9, D4, D6, D10).
 *
 * Process
 *   player_tick() spawns (no shell):
 *     <mpv_path> --idle=yes --no-video --no-terminal --audio-display=no
 *                --input-ipc-server=<socket>
 *   stdin is /dev/null. The child gets XDG_RUNTIME_DIR=/run/user/<getuid()>
 *   when the kiosk's environment has none (D6), so mpv finds PipeWire.
 *   Socket: player_config_t.socket_path, else $CHATBOX_MPV_SOCKET, else
 *   PLAYER_DEFAULT_SOCKET. Before each spawn a leftover mpv still listening
 *   on that path (e.g. orphaned by a crashed kiosk) is told to quit.
 *
 * Supervision (no SIGCHLD handler)
 *   Death is seen by waitpid(WNOHANG) from the tick and by EOF on the socket.
 *   Also treated as death: no connection within PLAYER_CONNECT_TIMEOUT_MS of
 *   the spawn, a request unanswered for PLAYER_REPLY_TIMEOUT_MS, or the
 *   write ring overflowing (mpv hung). The child is SIGKILLed and reaped
 *   later, never waited for. Respawn backoff: 0.5, 1, 2, 5 s, then every 5 s
 *   (§9); the backoff resets after a connection has stayed up
 *   PLAYER_STABLE_MS. A failed spawn (no mpv binary, as on a dev machine without it) is
 *   logged once and retried on the same schedule, so at most one attempt
 *   every 5 s: no busy loop, and the tick never blocks.
 *   After every (re)connect: observe the properties, set the volume, and if
 *   a track is current and not stopped, load it paused; on `file-loaded`
 *   seek to the saved position and restore playing/paused (§9, §12: no
 *   `start=` loadfile option, it isn't on every mpv version).
 *
 * State is intent
 *   The player's state is what the kid asked for, whether or not mpv is up
 *   right now. With mpv down or missing, every operation still updates the
 *   state (a queue started while mpv is missing reads PLAYING, position 0)
 *   and the next connection applies it. player_available() tells the UI
 *   whether sound can actually come out.
 *
 * Queue (§7.3, §9)
 *   Owned here, not by mpv's playlist: ids AND paths are copied in, so a
 *   library reload never touches it (§7.7). One file at a time with
 *   `loadfile <path> replace`.
 *   - end-file reason "eof"   → the next track (keeps playing), or, after
 *     the last one, stop.
 *   - end-file reason "error" → logged, skip to the next track; an error on
 *     the last track stops. So an all-bad queue walks to the end once and
 *     stops (bounded; nothing retries until the kid presses a key).
 *   - end-file "error" whose file_error is about the audio output (e.g.
 *     "audio output initialization failed": PipeWire unreachable) is NOT a
 *     bad file: the track, index and position stay, the state goes to
 *     PAUSED (logged once, with a PipeWire/XDG_RUNTIME_DIR hint), and the
 *     next play/toggle — or a respawn — loads it again.
 *   - "stop", "quit", "redirect" and anything else → ignored ("stop" is our
 *     own loadfile/stop). An end-file that arrives before the reply to our
 *     latest loadfile/stop is about the previous file and is ignored.
 *   - Stopped = the end of the queue: the last track stays current, position
 *     0, not playing (`stop` is sent to mpv). player_resume()/player_restart()
 *     reload it from 0:00; player_toggle() (Space) leaves it stopped.
 *
 * Callbacks
 *   Fire only from player_tick(), never from inside an operation, so a UI
 *   handler may call anything here. Position changes are coalesced to at
 *   most one callback per PLAYER_POS_INTERVAL_MS (the latest value is always
 *   in player_position_ms()).
 *
 * Threading: single-threaded, LVGL thread only. Allocation: cJSON for each
 * command/event line (like matrix_api); the queue, the path pool, the
 * socket buffers and the request table are static.
 */

#ifndef CHATBOX_PLAYER_H
#define CHATBOX_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define PLAYER_DEFAULT_SOCKET      "/run/chatbox/mpv.sock"
#define PLAYER_SOCKET_ENV          "CHATBOX_MPV_SOCKET"
#define PLAYER_DEFAULT_MPV         "mpv"

#define PLAYER_MAX_QUEUE           1024          /* = LIBRARY_MAX_ARTIST_TRACKS */
#define PLAYER_PATH_POOL           (256 * 1024)  /* bytes for all queue paths   */

#define PLAYER_VOLUME_STEP         5
#define PLAYER_PREV_RESTART_MS     3000   /* iPod rule: > this → restart     */
#define PLAYER_POS_INTERVAL_MS     250    /* ≤ 4 position callbacks / s      */
#define PLAYER_CONNECT_TIMEOUT_MS  5000
#define PLAYER_REPLY_TIMEOUT_MS    10000
#define PLAYER_STABLE_MS           10000
#define PLAYER_CONNECT_RETRY_MS    100    /* connect() attempts while starting */

/* Test builds only: extra mpv args from this env var (space-separated, at
 * most PLAYER_TEST_EXTRA_ARGS), e.g. "--ao=null". Ignored in the firmware. */
#define PLAYER_TEST_EXTRA_ENV      "CHATBOX_TEST_MPV_EXTRA_ARGS"
#define PLAYER_TEST_EXTRA_ARGS     4

typedef enum {
    PLAYER_IDLE = 0,    /* nothing loaded (empty queue)                        */
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_STOPPED,     /* end of queue: last track current, 0:00, not playing */
} player_state_t;

/* Result of player_play_song(). */
typedef enum {
    PLAYER_SONG_STARTED = 0,    /* not current: queue replaced, playing       */
    PLAYER_SONG_RESUMED,        /* current and paused/stopped: resumed        */
    PLAYER_SONG_ALREADY_PLAYING /* current and playing: nothing              */
} player_song_result_t;

typedef struct {
    int64_t     id;     /* library track id (> 0) */
    const char *path;   /* absolute path; copied  */
} player_track_t;

/* Process operations, injectable for tests. A handle is a pid (> 0). */
typedef struct {
    /* Start path with argv/envp. Return the handle, or -1 with errno set. */
    pid_t (*spawn)(const char *path, char *const argv[], char *const envp[], void *ud);
    /* true while running. Must not block; reaps the process once it's gone. */
    bool  (*alive)(pid_t h, void *ud);
    /* Kill it hard (SIGKILL). */
    void  (*kill)(pid_t h, void *ud);
    void  *ud;
} player_proc_ops_t;

typedef struct {
    const char *mpv_path;       /* NULL → PLAYER_DEFAULT_MPV (PATH lookup)  */
    const char *socket_path;    /* NULL → $CHATBOX_MPV_SOCKET or default     */
    int         volume_max;     /* music_volume_max, clamped to 0..100       */
    int         volume_default; /* music_volume_default, clamped to 0..max   */
    uint32_t  (*now_ms)(void);  /* NULL → monotonic clock (wrap-safe)        */
    const player_proc_ops_t *proc; /* NULL → posix_spawnp/waitpid/kill       */
} player_config_t;

typedef struct {
    void (*track_changed)(void *ud);    /* current track / index changed      */
    void (*state_changed)(void *ud);    /* playing/paused/stopped/idle, or
                                           player_available() changed         */
    void (*volume_changed)(void *ud);
    void (*position_changed)(void *ud); /* position or duration, coalesced    */
    void *ud;
} player_callbacks_t;

/** Reset all state and remember the config. Spawns nothing (the first
 *  player_tick() does). Callable again after player_deinit(). */
void player_init(const player_config_t *cfg);

/** Tell mpv to quit, kill and reap it (bounded, for exit only). */
void player_deinit(void);

/** Drive everything: spawn, connect, I/O, callbacks. From an lv_timer
 *  (~16 ms). Never blocks. */
void player_tick(void);

void player_set_callbacks(const player_callbacks_t *cb);

/* ---- operations (D4, §7.3, §8.2–§8.4) ---- */

/** Replace the queue and play tracks[start] from 0:00, even if it is the
 *  current track (Enter on an album or a song in the columns: Enter always
 *  plays from the start). n == 0 → no-op, false. */
bool player_play_queue(const player_track_t *tracks, int n, int start);

/** Play tracks[start] without restarting it: current → resume if not playing
 *  (the queue is kept; never a restart), else player_play_queue(). No key
 *  uses it (→ on a song does nothing, D4; Enter uses player_play_queue()). */
player_song_result_t player_play_song(const player_track_t *tracks, int n, int start);

/** Space: playing ↔ paused. Stopped (end of queue) and idle → nothing:
 *  Space never replays the queue. */
void player_toggle(void);

/** Paused → play; stopped → the last track again from 0:00; playing or
 *  idle → nothing. */
void player_resume(void);

/** Enter on Now Playing: the current track from 0:00, playing, whatever the
 *  state (playing, paused, stopped). Idle → nothing. */
void player_restart(void);

/** ← on Now Playing, iPod rule: position > 3 s or first track → restart
 *  from 0:00, else the previous track from 0:00. Keeps playing/paused.
 *  Stopped counts as paused at 0:00. */
void player_prev(void);

/** → on Now Playing: the next track from 0:00 (keeps playing/paused), true.
 *  On the last track, stopped or idle → nothing, false (the UI bounces; the
 *  song plays on). Only the natural end of the queue stops. */
bool player_next(void);

/** PgUp/PgDn: volume += delta (±PLAYER_VOLUME_STEP), clamped to
 *  0..volume_max. Returns the new volume. */
int player_volume_step(int delta);

/* ---- state ---- */

player_state_t player_state(void);
bool     player_is_playing(void);
int64_t  player_current_id(void);       /* 0 when idle                      */
const char *player_current_path(void);  /* "" when idle                     */
int      player_current_index(void);    /* -1 when idle                     */
int      player_queue_len(void);
int64_t  player_queue_id(int i);        /* 0 when out of range              */
int64_t  player_position_ms(void);
int64_t  player_duration_ms(void);      /* -1 = unknown                     */
int      player_volume(void);
int      player_volume_max(void);
bool     player_available(void);        /* connected to a live mpv          */
const char *player_socket_path(void);

#ifdef CHATBOX_TESTING
int   player_test_fd(void);             /* IPC socket, -1 when not connected */
pid_t player_test_pid(void);            /* child handle, 0 when none         */
int   player_test_spawn_count(void);    /* spawn attempts (incl. failures)   */
int   player_test_pending(void);        /* requests awaiting a reply         */
size_t player_test_tx_queued(void);     /* bytes waiting in the write ring   */
#endif

#endif /* CHATBOX_PLAYER_H */
