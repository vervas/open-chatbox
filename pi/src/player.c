/**
 * player.c — Music player core: queue + mpv child over JSON IPC
 * (docs/player-spec.md §9). Contract and semantics: player.h.
 *
 * Everything is driven by player_tick() on the LVGL thread. The socket is
 * non-blocking; the only syscalls per idle tick are one waitpid(WNOHANG)
 * and one recv() that returns EAGAIN.
 */

#include "player.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

extern char **environ;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0      /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

#define RX_BUF       16384          /* longest line we accept from mpv       */
#define TX_BUF       32768          /* write ring: commands mpv hasn't read  */
/* Requests awaiting a reply. Sized above what the kernel socket buffer
 * (~200 KB on Linux) plus the ring can hold of normal traffic, so a slow
 * reader is caught by the ring or the reply timeout, not by this table. */
#define MAX_REQS     256
#define REAP_SLOTS   4              /* killed children not yet reaped        */
#define READS_PER_TICK 64           /* bound the work of one tick            */

/* observe_property ids */
#define OBS_TIME_POS 1
#define OBS_DURATION 2
#define OBS_PAUSE    3
#define OBS_IDLE     4

/* Respawn backoff (§9): 0.5, 1, 2, 5 s, then every 5 s. */
static const uint32_t k_backoff_ms[] = { 500, 1000, 2000, 5000 };
#define BACKOFF_STEPS ((int)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0])))

/* Dirty bits → callbacks, fired from player_tick() only. */
#define DIRTY_TRACK   0x01u
#define DIRTY_STATE   0x02u
#define DIRTY_VOLUME  0x04u
#define DIRTY_POS     0x08u   /* time-pos moved: rate-limited               */
#define DIRTY_POS_NOW 0x10u   /* explicit jump (load, restart, stop): now   */

/* -------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------- */

typedef enum { CONN_DOWN = 0, CONN_STARTING, CONN_CONNECTED } conn_t;

typedef enum { REQ_FREE = 0, REQ_LOAD, REQ_STOP, REQ_SEEK, REQ_OTHER } req_kind_t;

typedef struct {
    uint32_t   id;
    req_kind_t kind;
    uint32_t   sent_ms;
} req_t;

typedef struct {
    int64_t  id;
    uint32_t off;           /* into g_pool */
} qent_t;

/* config */
static char              g_mpv_path[PATH_MAX];
static char              g_sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static uint32_t        (*g_now)(void);
static player_proc_ops_t g_ops;
static bool              g_inited = false;

/* queue + intent */
static qent_t         g_q[PLAYER_MAX_QUEUE];
static char           g_pool[PLAYER_PATH_POOL];
static int            g_qn  = 0;
static int            g_idx = -1;
static player_state_t g_state = PLAYER_IDLE;
static int64_t        g_pos_ms = 0;
static int64_t        g_dur_ms = -1;
static int            g_vol = 0, g_vol_max = 100;

/* process + connection */
static conn_t   g_conn = CONN_DOWN;
static pid_t    g_pid = 0;
static int      g_fd = -1;
static int      g_connecting_fd = -1;   /* connect() returned EINPROGRESS */
static uint32_t g_next_spawn_ms, g_spawned_ms, g_next_connect_ms, g_connected_ms;
static int      g_backoff_i = 0;
static bool     g_stable = false;
static int      g_spawn_count = 0;
static bool     g_spawn_fail_logged = false;
static pid_t    g_reap[REAP_SLOTS];

/* I/O */
static char     g_rx[RX_BUF];
static size_t   g_rx_len = 0;
static bool     g_rx_discard = false;   /* dropping the rest of a long line */
static char     g_tx[TX_BUF];
static size_t   g_tx_head = 0, g_tx_len = 0;
static req_t    g_req[MAX_REQS];
static uint32_t g_next_req_id = 0;
static int      g_load_pending = 0;     /* loadfile/stop replies outstanding */
static int      g_seek_pending = 0;
static bool     g_restore = false;      /* reloaded after (re)connect: seek on file-loaded */
static bool     g_need_reload = false;  /* audio output failed: mpv has no file loaded */
static bool     g_ao_logged = false;

/* callbacks */
static player_callbacks_t g_cb;
static unsigned g_dirty = 0;
static uint32_t g_last_pos_cb = 0;

/* -------------------------------------------------------------------------
 * Clock and default process operations
 * ---------------------------------------------------------------------- */

static uint32_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* Wrap-safe "t has come". */
static bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

/* Wrap-safe ms since t; 0 when t is later than now (a timestamp taken
 * inside this tick, after `now` was read). */
static uint32_t since(uint32_t now, uint32_t t)
{
    int32_t d = (int32_t)(now - t);
    return d > 0 ? (uint32_t)d : 0;
}

static pid_t sys_spawn(const char *path, char *const argv[], char *const envp[], void *ud)
{
    (void)ud;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_init(&attr);
    /* A clean signal state for mpv, whatever the kiosk ignores or blocks
     * (ignored signals survive exec; main.c ignores SIGINT). */
    sigset_t none, dflt;
    sigemptyset(&none);
    sigemptyset(&dflt);
    sigaddset(&dflt, SIGPIPE);
    sigaddset(&dflt, SIGCHLD);
    sigaddset(&dflt, SIGINT);
    sigaddset(&dflt, SIGTERM);
    sigaddset(&dflt, SIGHUP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dflt);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, path, &fa, &attr, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return pid;
}

static bool sys_alive(pid_t h, void *ud)
{
    (void)ud;
    int st;
    pid_t r = waitpid(h, &st, WNOHANG);
    if (r == 0) return true;
    if (r < 0 && errno == EINTR) return true;   /* ask again next tick */
    return false;                               /* reaped, or ECHILD   */
}

static void sys_kill(pid_t h, void *ud)
{
    (void)ud;
    if (h > 0) kill(h, SIGKILL);
}

static const player_proc_ops_t k_sys_ops = { sys_spawn, sys_alive, sys_kill, NULL };

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

static bool has_track(void) { return g_idx >= 0 && g_idx < g_qn; }

static bool loaded_state(void)
{
    return g_state == PLAYER_PLAYING || g_state == PLAYER_PAUSED;
}

static const char *qpath(int i) { return g_pool + g_q[i].off; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_nonblock_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

/* Non-blocking unix-socket connect. Returns the fd (connected, or pending
 * when *pending), or -1. */
static int unix_connect(bool *pending)
{
    *pending = false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_nonblock_cloexec(fd);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, g_sock_path, strlen(g_sock_path) + 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) return fd;
    if (errno == EINPROGRESS) {
        *pending = true;
        return fd;
    }
    close(fd);
    return -1;
}

/* -------------------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------------- */

static void reap_add(pid_t h)
{
    for (int i = 0; i < REAP_SLOTS; i++) {
        if (g_reap[i] == 0) { g_reap[i] = h; return; }
    }
    fprintf(stderr, "player: reap list full, pid %d left to init\n", (int)h);
}

static void reap_tick(void)
{
    for (int i = 0; i < REAP_SLOTS; i++) {
        if (g_reap[i] && !g_ops.alive(g_reap[i], g_ops.ud)) g_reap[i] = 0;
    }
}

static void io_reset(void)
{
    g_rx_len = 0;
    g_rx_discard = false;
    g_tx_head = g_tx_len = 0;
    memset(g_req, 0, sizeof(g_req));
    g_load_pending = g_seek_pending = 0;
    g_restore = false;
    g_need_reload = false;      /* a (re)connect reloads the current track */
}

static void schedule_respawn(uint32_t now)
{
    g_conn = CONN_DOWN;
    uint32_t d = k_backoff_ms[g_backoff_i < BACKOFF_STEPS ? g_backoff_i : BACKOFF_STEPS - 1];
    if (g_backoff_i < BACKOFF_STEPS) g_backoff_i++;
    g_next_spawn_ms = now + d;
}

/* Close the socket, kill the child if it's still running, back off. The
 * player's state (intent) is untouched: the next connection restores it. */
static void conn_drop(const char *why)
{
    uint32_t now = g_now();
    bool was_up = (g_conn == CONN_CONNECTED);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    if (g_connecting_fd >= 0) { close(g_connecting_fd); g_connecting_fd = -1; }
    if (g_pid > 0) {
        g_ops.kill(g_pid, g_ops.ud);
        reap_add(g_pid);
        g_pid = 0;
    }
    io_reset();
    schedule_respawn(now);
    fprintf(stderr, "player: mpv lost (%s), respawn in %u ms\n",
            why, (unsigned)(g_next_spawn_ms - now));
    if (was_up) g_dirty |= DIRTY_STATE;     /* player_available() changed */
}

/* A kiosk that crashed leaves its mpv running (still playing, still on the
 * socket). Ask whatever is listening on our path to quit before a spawn. */
static void quit_orphan(void)
{
    bool pending;
    int fd = unix_connect(&pending);
    if (fd < 0) return;
    if (!pending) {
        static const char q[] = "{\"command\":[\"quit\"]}\n";
        if (send(fd, q, sizeof(q) - 1, MSG_NOSIGNAL) > 0)
            fprintf(stderr, "player: told a leftover mpv on %s to quit\n", g_sock_path);
    }
    close(fd);
}

static char **build_env(char *xdg, size_t xdg_size)
{
    size_t n = 0;
    while (environ && environ[n]) n++;
    char **env = malloc((n + 2) * sizeof(char *));
    if (!env) return NULL;
    const char *cur = getenv("XDG_RUNTIME_DIR");
    bool need = (cur == NULL || cur[0] == '\0');
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (need && strncmp(environ[i], "XDG_RUNTIME_DIR=", 16) == 0) continue;
        env[k++] = environ[i];
    }
    if (need) {
        snprintf(xdg, xdg_size, "XDG_RUNTIME_DIR=/run/user/%u", (unsigned)getuid());
        env[k++] = xdg;
    }
    env[k] = NULL;
    return env;
}

static void do_spawn(uint32_t now)
{
    g_spawn_count++;
    quit_orphan();

    char ipc[sizeof(g_sock_path) + 32];
    snprintf(ipc, sizeof(ipc), "--input-ipc-server=%s", g_sock_path);
    char *argv[6 + PLAYER_TEST_EXTRA_ARGS + 1] = {
        g_mpv_path,
        "--idle=yes",
        "--no-video",
        "--no-terminal",
        "--audio-display=no",
        ipc,
        NULL
    };
#ifdef CHATBOX_TESTING
    /* Test builds only: extra space-separated args (e.g. --ao=null for the
     * real-mpv tests). The firmware's argv is exactly §9's. */
    char extra[512] = "";
    const char *ex = getenv(PLAYER_TEST_EXTRA_ENV);
    if (ex) snprintf(extra, sizeof(extra), "%s", ex);
    int an = 6;
    for (char *tok = strtok(extra, " "); tok && an < 6 + PLAYER_TEST_EXTRA_ARGS;
         tok = strtok(NULL, " "))
        argv[an++] = tok;
    argv[an] = NULL;
#endif
    char xdg[64];
    char **env = build_env(xdg, sizeof(xdg));
    pid_t pid = env ? g_ops.spawn(g_mpv_path, argv, env, g_ops.ud) : -1;
    int err = errno;
    free(env);

    if (pid <= 0) {
        if (!g_spawn_fail_logged) {
            fprintf(stderr, "player: cannot start %s (%s); music is silent, "
                    "retrying every %u s\n", g_mpv_path, strerror(err),
                    (unsigned)(k_backoff_ms[BACKOFF_STEPS - 1] / 1000));
            g_spawn_fail_logged = true;
        }
        schedule_respawn(now);
        return;
    }
    g_spawn_fail_logged = false;
    fprintf(stderr, "player: started %s (pid %d), socket %s\n",
            g_mpv_path, (int)pid, g_sock_path);
    g_pid = pid;
    g_conn = CONN_STARTING;
    g_spawned_ms = now;
    g_next_connect_ms = now;
}

/* -------------------------------------------------------------------------
 * Writing commands
 * ---------------------------------------------------------------------- */

static void flush_tx(void)
{
    while (g_conn == CONN_CONNECTED && g_tx_len > 0) {
        size_t chunk = g_tx_len;
        if (chunk > TX_BUF - g_tx_head) chunk = TX_BUF - g_tx_head;
        ssize_t n = send(g_fd, g_tx + g_tx_head, chunk, MSG_NOSIGNAL);
        if (n > 0) {
            g_tx_head = (g_tx_head + (size_t)n) % TX_BUF;
            g_tx_len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; /* next tick */
        conn_drop("write failed");
        return;
    }
}

static void tx_push(const char *s, size_t len)
{
    size_t tail = (g_tx_head + g_tx_len) % TX_BUF;
    for (size_t i = 0; i < len; i++) {
        g_tx[tail] = s[i];
        tail = (tail + 1) % TX_BUF;
    }
    g_tx_len += len;
}

/* Send {"command": cmd, "request_id": N}. Takes ownership of `cmd`.
 * Not connected → dropped (the next connection restores the state). */
static bool send_cmd(req_kind_t kind, cJSON *cmd)
{
    if (!cmd) return false;
    if (g_conn != CONN_CONNECTED) {
        cJSON_Delete(cmd);
        return false;
    }
    int slot = -1;
    for (int i = 0; i < MAX_REQS; i++) {
        if (g_req[i].kind == REQ_FREE) { slot = i; break; }
    }
    if (slot < 0) {
        cJSON_Delete(cmd);
        conn_drop("too many unanswered requests");
        return false;
    }
    if (++g_next_req_id == 0) g_next_req_id = 1;
    uint32_t id = g_next_req_id;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) { cJSON_Delete(cmd); return false; }
    cJSON_AddItemToObject(obj, "command", cmd);
    cJSON_AddNumberToObject(obj, "request_id", (double)id);
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return false;
    size_t len = strlen(s);
    if (len + 1 > TX_BUF - g_tx_len) {
        free(s);
        conn_drop("write ring full, mpv isn't reading");
        return false;
    }
    tx_push(s, len);
    tx_push("\n", 1);
    free(s);

    g_req[slot].id = id;
    g_req[slot].kind = kind;
    g_req[slot].sent_ms = g_now();
    if (kind == REQ_LOAD || kind == REQ_STOP) g_load_pending++;
    if (kind == REQ_SEEK) g_seek_pending++;
    flush_tx();
    return true;
}

static cJSON *cmd_strs(const char *a, const char *b, const char *c)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON_AddItemToArray(arr, cJSON_CreateString(a));
    if (b) cJSON_AddItemToArray(arr, cJSON_CreateString(b));
    if (c) cJSON_AddItemToArray(arr, cJSON_CreateString(c));
    return arr;
}

static void send_pause(bool paused)
{
    cJSON *arr = cmd_strs("set_property", "pause", NULL);
    if (arr) cJSON_AddItemToArray(arr, cJSON_CreateBool(paused));
    send_cmd(REQ_OTHER, arr);
}

static void send_volume(void)
{
    cJSON *arr = cmd_strs("set_property", "volume", NULL);
    if (arr) cJSON_AddItemToArray(arr, cJSON_CreateNumber(g_vol));
    send_cmd(REQ_OTHER, arr);
}

static void send_seek(int64_t ms)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    cJSON_AddItemToArray(arr, cJSON_CreateString("seek"));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)ms / 1000.0));
    cJSON_AddItemToArray(arr, cJSON_CreateString("absolute"));
    send_cmd(REQ_SEEK, arr);
}

static void send_load(void)
{
    /* The path goes in as a JSON string: cJSON escapes quotes, backslashes
     * and control characters; UTF-8 passes through. */
    send_cmd(REQ_LOAD, cmd_strs("loadfile", qpath(g_idx), "replace"));
}

static void send_observe(int id, const char *name)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    cJSON_AddItemToArray(arr, cJSON_CreateString("observe_property"));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(id));
    cJSON_AddItemToArray(arr, cJSON_CreateString(name));
    send_cmd(REQ_OTHER, arr);
}

/* -------------------------------------------------------------------------
 * Queue transitions
 * ---------------------------------------------------------------------- */

/* Reload g_idx at g_pos_ms (after an audio-output failure left mpv with
 * nothing loaded): load paused, seek + set pause on file-loaded, as a
 * restore does. Not connected → the next connection does it anyway. */
static void reload_current(void)
{
    g_need_reload = false;
    if (g_conn != CONN_CONNECTED) return;
    send_pause(true);
    send_load();
    g_restore = (g_conn == CONN_CONNECTED);
}

/* Load g_idx from 0:00, playing or paused. Pause is set first: mpv keeps
 * the pause property across loadfile, so a paused load makes no sound. */
static void load_current(bool play, bool track_changed)
{
    g_state = play ? PLAYER_PLAYING : PLAYER_PAUSED;
    g_pos_ms = 0;
    if (track_changed) g_dur_ms = -1;
    g_restore = false;
    g_need_reload = false;
    g_dirty |= DIRTY_STATE | DIRTY_POS_NOW | (track_changed ? DIRTY_TRACK : 0);
    send_pause(!play);
    send_load();
}

/* End of the queue: last track current, 0:00, not playing. */
static void do_stop(void)
{
    g_state = PLAYER_STOPPED;
    g_pos_ms = 0;
    g_restore = false;
    g_need_reload = false;
    g_dirty |= DIRTY_STATE | DIRTY_POS_NOW;
    send_cmd(REQ_STOP, cmd_strs("stop", NULL, NULL));
}

/* eof / error / player_next: the next track, keeping playing/paused, or
 * (eof / error on the last track only) stop. */
static void advance(void)
{
    if (g_idx < g_qn - 1) {
        g_idx++;
        load_current(g_state == PLAYER_PLAYING, true);
    } else {
        do_stop();
    }
}

/* -------------------------------------------------------------------------
 * Reading replies and events
 * ---------------------------------------------------------------------- */

static void on_connected(int fd, uint32_t now)
{
    g_fd = fd;
    g_conn = CONN_CONNECTED;
    g_connected_ms = now;
    g_stable = false;
    io_reset();
    g_dirty |= DIRTY_STATE;
    fprintf(stderr, "player: connected to mpv\n");

    send_observe(OBS_TIME_POS, "time-pos");
    send_observe(OBS_DURATION, "duration");
    send_observe(OBS_PAUSE, "pause");
    send_observe(OBS_IDLE, "idle-active");
    send_volume();
    if (loaded_state() && has_track()) {
        /* Restore: load paused, seek to g_pos_ms on file-loaded, then set
         * the pause state the kid wants by then (§9). */
        send_pause(true);
        send_load();
        g_restore = (g_conn == CONN_CONNECTED);
    }
}

static void handle_reply(uint32_t id, const char *err)
{
    int slot = -1;
    for (int i = 0; i < MAX_REQS; i++) {
        if (g_req[i].kind != REQ_FREE && g_req[i].id == id) { slot = i; break; }
    }
    if (slot < 0) return;                       /* not ours / already gone */
    req_kind_t kind = g_req[slot].kind;
    g_req[slot].kind = REQ_FREE;
    if (kind == REQ_LOAD || kind == REQ_STOP) g_load_pending--;
    if (kind == REQ_SEEK) g_seek_pending--;

    if (err && strcmp(err, "success") != 0) {
        fprintf(stderr, "player: mpv request %u failed: %s\n", (unsigned)id, err);
        /* A refused loadfile of the current track acts like an error
         * end-file, so the queue doesn't stall on it. */
        if (kind == REQ_LOAD && g_load_pending == 0 && loaded_state() && has_track()) {
            fprintf(stderr, "player: skipping track %lld\n", (long long)g_q[g_idx].id);
            advance();
        }
    }
}

/* mpv's file_error strings that are about the audio device, not the file
 * (MPV_ERROR_AO_INIT_FAILED = "audio output initialization failed"). */
static bool is_output_error(const char *msg)
{
    return strstr(msg, "audio output") != NULL || strstr(msg, "audio device") != NULL;
}

static void handle_end_file(cJSON *ev)
{
    /* Before the reply to our latest loadfile/stop, an end-file is about
     * the previous file. And there's nothing to advance when stopped. */
    if (g_load_pending > 0 || !loaded_state() || !has_track()) return;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(ev, "reason");
    const char *reason = cJSON_IsString(r) ? r->valuestring : "";
    if (strcmp(reason, "eof") == 0) {
        advance();
    } else if (strcmp(reason, "error") == 0) {
        const cJSON *fe = cJSON_GetObjectItemCaseSensitive(ev, "file_error");
        const char *msg = cJSON_IsString(fe) ? fe->valuestring : "error";
        if (is_output_error(msg)) {
            /* Not the file's fault: skipping would run through the whole
             * album. Keep track, index and position; pause; the next play
             * (or a respawn) tries again. */
            if (!g_ao_logged) {
                fprintf(stderr, "player: mpv audio output failed (%s): is PipeWire running "
                        "for this user and XDG_RUNTIME_DIR right (D6)? Paused; retrying "
                        "on the next play\n", msg);
                g_ao_logged = true;
            }
            g_state = PLAYER_PAUSED;
            g_restore = false;
            g_need_reload = true;
            g_dirty |= DIRTY_STATE;
            return;
        }
        fprintf(stderr, "player: can't play track %lld (%s): %s, skipping\n",
                (long long)g_q[g_idx].id, qpath(g_idx), msg);
        advance();
    }
    /* stop / quit / redirect / unknown: ignored. */
}

static void handle_file_loaded(void)
{
    if (g_load_pending > 0 || !g_restore) return;
    g_restore = false;
    if (g_pos_ms > 0) send_seek(g_pos_ms);
    if (g_state == PLAYER_PLAYING) send_pause(false);
}

static void handle_property(cJSON *ev)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(ev, "name");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
    if (!cJSON_IsString(name)) return;
    /* Values from mpv only count once our latest load/seek/restore has
     * landed; before that they describe the previous file or position. */
    bool settled = loaded_state() && g_load_pending == 0 && !g_restore;

    if (strcmp(name->valuestring, "time-pos") == 0) {
        if (settled && g_seek_pending == 0 && cJSON_IsNumber(data) && data->valuedouble >= 0) {
            int64_t ms = (int64_t)(data->valuedouble * 1000.0);
            if (ms != g_pos_ms) { g_pos_ms = ms; g_dirty |= DIRTY_POS; }
        }
    } else if (strcmp(name->valuestring, "duration") == 0) {
        if (settled && cJSON_IsNumber(data) && data->valuedouble >= 0) {
            int64_t ms = (int64_t)(data->valuedouble * 1000.0);
            if (ms != g_dur_ms) { g_dur_ms = ms; g_dirty |= DIRTY_POS; }
        }
    }
    /* "pause" and "idle-active" are observed for the log only: the player's
     * state is the kid's intent and is pushed to mpv, not read back. */
}

static void handle_line(const char *line, size_t len)
{
    cJSON *obj = cJSON_ParseWithLength(line, len);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return;
    }
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(obj, "event");
    if (cJSON_IsString(ev)) {
        const char *e = ev->valuestring;
        if (strcmp(e, "property-change") == 0)   handle_property(obj);
        else if (strcmp(e, "end-file") == 0)     handle_end_file(obj);
        else if (strcmp(e, "file-loaded") == 0)  handle_file_loaded();
        else if (strcmp(e, "playback-restart") == 0) g_ao_logged = false; /* sound flows */
    } else {
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(obj, "request_id");
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(obj, "error");
        if (cJSON_IsNumber(rid) && rid->valuedouble > 0)
            handle_reply((uint32_t)rid->valuedouble,
                         cJSON_IsString(err) ? err->valuestring : NULL);
    }
    cJSON_Delete(obj);
}

/* Read what's there, split into lines. A line longer than RX_BUF is
 * dropped whole, and parsing resumes at the next newline. */
static void read_rx(void)
{
    for (int r = 0; r < READS_PER_TICK && g_conn == CONN_CONNECTED; r++) {
        ssize_t n = recv(g_fd, g_rx + g_rx_len, RX_BUF - g_rx_len, 0);
        if (n == 0) { conn_drop("socket closed"); return; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) conn_drop("read failed");
            return;
        }
        g_rx_len += (size_t)n;

        size_t start = 0;
        for (size_t i = 0; i < g_rx_len; i++) {
            if (g_rx[i] != '\n') continue;
            if (g_rx_discard) g_rx_discard = false;     /* tail of a long line */
            else if (i > start) handle_line(g_rx + start, i - start);
            if (g_conn != CONN_CONNECTED) return;       /* dropped meanwhile */
            start = i + 1;
        }
        if (start > 0) {
            memmove(g_rx, g_rx + start, g_rx_len - start);
            g_rx_len -= start;
        }
        if (g_rx_len == RX_BUF) {
            if (!g_rx_discard)
                fprintf(stderr, "player: mpv line over %d bytes, dropped\n", RX_BUF);
            g_rx_discard = true;
            g_rx_len = 0;
        } else if (g_rx_discard) {
            g_rx_len = 0;       /* still inside the long line */
        }
    }
}

/* -------------------------------------------------------------------------
 * Public: lifecycle
 * ---------------------------------------------------------------------- */

void player_init(const player_config_t *cfg)
{
    player_config_t c;
    memset(&c, 0, sizeof(c));
    if (cfg) c = *cfg;

    snprintf(g_mpv_path, sizeof(g_mpv_path), "%s", c.mpv_path ? c.mpv_path : PLAYER_DEFAULT_MPV);
    const char *sock = c.socket_path;
    if (!sock || !*sock) sock = getenv(PLAYER_SOCKET_ENV);
    if (!sock || !*sock) sock = PLAYER_DEFAULT_SOCKET;
    if (strlen(sock) >= sizeof(g_sock_path))
        fprintf(stderr, "player: socket path too long (max %zu): %s\n",
                sizeof(g_sock_path) - 1, sock);
    snprintf(g_sock_path, sizeof(g_sock_path), "%s", sock);
    g_now = c.now_ms ? c.now_ms : mono_ms;
    g_ops = c.proc ? *c.proc : k_sys_ops;

    g_vol_max = clampi(c.volume_max, 0, 100);
    g_vol = clampi(c.volume_default, 0, g_vol_max);

    g_qn = 0;
    g_idx = -1;
    g_state = PLAYER_IDLE;
    g_pos_ms = 0;
    g_dur_ms = -1;

    g_conn = CONN_DOWN;
    g_pid = 0;
    g_fd = g_connecting_fd = -1;
    g_backoff_i = 0;
    g_stable = false;
    g_spawn_count = 0;
    g_spawn_fail_logged = false;
    g_ao_logged = false;
    memset(g_reap, 0, sizeof(g_reap));
    io_reset();
    g_next_req_id = 0;

    memset(&g_cb, 0, sizeof(g_cb));
    g_dirty = 0;
    g_next_spawn_ms = g_now();          /* first tick spawns */
    g_last_pos_cb = g_next_spawn_ms - PLAYER_POS_INTERVAL_MS;
    g_inited = true;
}

void player_deinit(void)
{
    if (!g_inited) return;
    if (g_conn == CONN_CONNECTED) {
        static const char q[] = "{\"command\":[\"quit\"]}\n";
        (void)!send(g_fd, q, sizeof(q) - 1, MSG_NOSIGNAL);
    }
    if (g_fd >= 0) close(g_fd);
    if (g_connecting_fd >= 0) close(g_connecting_fd);
    g_fd = g_connecting_fd = -1;
    if (g_pid > 0) {
        g_ops.kill(g_pid, g_ops.ud);
        reap_add(g_pid);
        g_pid = 0;
    }
    /* SIGKILLed children go away at once; bound the wait anyway (exit only). */
    for (int t = 0; t < 200; t++) {
        reap_tick();
        bool any = false;
        for (int i = 0; i < REAP_SLOTS; i++) any |= (g_reap[i] != 0);
        if (!any) break;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    g_conn = CONN_DOWN;
    io_reset();
    g_inited = false;
}

void player_set_callbacks(const player_callbacks_t *cb)
{
    if (cb) g_cb = *cb;
    else memset(&g_cb, 0, sizeof(g_cb));
}

static void fire_callbacks(uint32_t now)
{
    unsigned d = g_dirty;
    bool pos = (d & DIRTY_POS_NOW) ||
               ((d & DIRTY_POS) && since(now, g_last_pos_cb) >= PLAYER_POS_INTERVAL_MS);
    g_dirty &= pos ? 0u : DIRTY_POS;    /* a throttled position waits */
    if ((d & DIRTY_TRACK) && g_cb.track_changed)   g_cb.track_changed(g_cb.ud);
    if ((d & DIRTY_STATE) && g_cb.state_changed)   g_cb.state_changed(g_cb.ud);
    if ((d & DIRTY_VOLUME) && g_cb.volume_changed) g_cb.volume_changed(g_cb.ud);
    if (pos) {
        g_last_pos_cb = now;
        if (g_cb.position_changed) g_cb.position_changed(g_cb.ud);
    }
}

void player_tick(void)
{
    if (!g_inited) return;
    uint32_t now = g_now();
    reap_tick();

    if (g_conn == CONN_DOWN && due(now, g_next_spawn_ms))
        do_spawn(now);

    if (g_conn == CONN_STARTING) {
        if (!g_ops.alive(g_pid, g_ops.ud)) {
            g_pid = 0;                          /* reaped by alive() */
            conn_drop("mpv exited before its socket was up");
        } else if (g_connecting_fd >= 0) {
            struct pollfd p = { g_connecting_fd, POLLOUT, 0 };
            if (poll(&p, 1, 0) > 0) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(g_connecting_fd, SOL_SOCKET, SO_ERROR, &err, &el);
                int fd = g_connecting_fd;
                g_connecting_fd = -1;
                if (err == 0) on_connected(fd, now);
                else { close(fd); g_next_connect_ms = now + PLAYER_CONNECT_RETRY_MS; }
            }
        } else if (due(now, g_next_connect_ms)) {
            bool pending;
            int fd = unix_connect(&pending);
            if (fd >= 0 && !pending) on_connected(fd, now);
            else if (fd >= 0) g_connecting_fd = fd;
            else g_next_connect_ms = now + PLAYER_CONNECT_RETRY_MS;
        }
        if (g_conn == CONN_STARTING && since(now, g_spawned_ms) >= PLAYER_CONNECT_TIMEOUT_MS)
            conn_drop("no IPC socket in time");
    }

    if (g_conn == CONN_CONNECTED) {
        if (!g_ops.alive(g_pid, g_ops.ud)) {
            g_pid = 0;
            conn_drop("mpv exited");
        }
    }
    if (g_conn == CONN_CONNECTED) read_rx();
    if (g_conn == CONN_CONNECTED) flush_tx();
    if (g_conn == CONN_CONNECTED) {
        for (int i = 0; i < MAX_REQS; i++) {
            if (g_req[i].kind != REQ_FREE &&
                since(now, g_req[i].sent_ms) >= PLAYER_REPLY_TIMEOUT_MS) {
                conn_drop("mpv stopped answering");
                break;
            }
        }
    }
    if (g_conn == CONN_CONNECTED && !g_stable &&
        since(now, g_connected_ms) >= PLAYER_STABLE_MS) {
        g_stable = true;
        g_backoff_i = 0;
    }

    fire_callbacks(now);
}

/* -------------------------------------------------------------------------
 * Public: operations
 * ---------------------------------------------------------------------- */

bool player_play_queue(const player_track_t *tracks, int n, int start)
{
    if (!g_inited || !tracks || n <= 0) return false;
    start = clampi(start, 0, n - 1);

    /* Copy ids and paths. If everything doesn't fit, keep a window that
     * starts at the chosen track (← then can't reach earlier ones). */
    size_t need = 0;
    for (int i = 0; i < n; i++) need += strlen(tracks[i].path ? tracks[i].path : "") + 1;
    int first = (n <= PLAYER_MAX_QUEUE && need <= PLAYER_PATH_POOL) ? 0 : start;
    if (first) fprintf(stderr, "player: queue of %d tracks too big, starting it at %d\n",
                       n, start);
    int qn = 0;
    size_t off = 0;
    for (int i = first; i < n && qn < PLAYER_MAX_QUEUE; i++) {
        const char *p = tracks[i].path ? tracks[i].path : "";
        size_t l = strlen(p) + 1;
        if (off + l > PLAYER_PATH_POOL) break;
        memcpy(g_pool + off, p, l);
        g_q[qn].id = tracks[i].id;
        g_q[qn].off = (uint32_t)off;
        off += l;
        qn++;
    }
    if (qn == 0) {
        fprintf(stderr, "player: track path too long, not played\n");
        return false;
    }
    if (qn < n - first)
        fprintf(stderr, "player: queue cut to %d tracks\n", qn);
    g_qn = qn;
    g_idx = start - first;
    load_current(true, true);
    return true;
}

player_song_result_t player_play_song(const player_track_t *tracks, int n, int start)
{
    if (tracks && n > 0 && g_state != PLAYER_IDLE && has_track()) {
        int s = clampi(start, 0, n - 1);
        if (tracks[s].id == g_q[g_idx].id) {
            if (g_state == PLAYER_PLAYING) return PLAYER_SONG_ALREADY_PLAYING;
            player_resume();
            return PLAYER_SONG_RESUMED;
        }
    }
    player_play_queue(tracks, n, start);
    return PLAYER_SONG_STARTED;
}

void player_toggle(void)
{
    /* Space (§8.2–§8.4): pause/resume only. Stopped and idle are no-ops:
     * Space never replays the end of the queue. */
    if (!g_inited || !has_track() || !loaded_state()) return;
    bool play = (g_state == PLAYER_PAUSED);
    g_state = play ? PLAYER_PLAYING : PLAYER_PAUSED;
    g_dirty |= DIRTY_STATE;
    if (play && g_need_reload) reload_current();
    /* During a restore the file-loaded handler applies it. */
    else if (!g_restore) send_pause(!play);
}

void player_resume(void)
{
    if (!g_inited || !has_track()) return;
    if (g_state == PLAYER_PAUSED) player_toggle();
    else if (g_state == PLAYER_STOPPED) load_current(true, false); /* last track from 0:00 */
}

void player_restart(void)
{
    if (!g_inited || !has_track() || g_state == PLAYER_IDLE) return;
    /* Reload rather than seek: it also plays a stopped track, retries one
     * whose audio output failed, and needs no restore bookkeeping. */
    load_current(true, false);
}

void player_prev(void)
{
    if (!g_inited || !has_track() || g_state == PLAYER_IDLE) return;
    if (g_state == PLAYER_STOPPED) {
        /* Stopped = paused at 0:00: restarting is a no-op, and the previous
         * track loads paused. */
        if (g_idx == 0) return;
        g_idx--;
        load_current(false, true);
        return;
    }
    if (g_pos_ms > PLAYER_PREV_RESTART_MS || g_idx == 0) {
        g_pos_ms = 0;
        g_dirty |= DIRTY_POS_NOW;
        /* A restore / reload seeks to g_pos_ms = 0 itself. */
        if (!g_restore && !g_need_reload) send_seek(0);
        return;
    }
    g_idx--;
    load_current(g_state == PLAYER_PLAYING, true);
}

bool player_next(void)
{
    /* The last track: nothing (the kid's song plays on); only the natural
     * end of the queue stops (advance() from end-file). */
    if (!g_inited || !has_track() || !loaded_state() || g_idx >= g_qn - 1) return false;
    advance();
    return true;
}

int player_volume_step(int delta)
{
    int v = clampi(g_vol + delta, 0, g_vol_max);
    if (v != g_vol) {
        g_vol = v;
        g_dirty |= DIRTY_VOLUME;
        send_volume();
    }
    return g_vol;
}

/* -------------------------------------------------------------------------
 * Public: state
 * ---------------------------------------------------------------------- */

player_state_t player_state(void)   { return g_state; }
bool     player_is_playing(void)    { return g_state == PLAYER_PLAYING; }
int64_t  player_current_id(void)    { return has_track() ? g_q[g_idx].id : 0; }
const char *player_current_path(void) { return has_track() ? qpath(g_idx) : ""; }
int      player_current_index(void) { return has_track() ? g_idx : -1; }
int      player_queue_len(void)     { return g_qn; }
int64_t  player_queue_id(int i)     { return (i >= 0 && i < g_qn) ? g_q[i].id : 0; }
int64_t  player_position_ms(void)   { return g_pos_ms; }
int64_t  player_duration_ms(void)   { return g_dur_ms; }
int      player_volume(void)        { return g_vol; }
int      player_volume_max(void)    { return g_vol_max; }
bool     player_available(void)     { return g_conn == CONN_CONNECTED; }
const char *player_socket_path(void) { return g_sock_path; }

#ifdef CHATBOX_TESTING
int    player_test_fd(void)          { return g_conn == CONN_CONNECTED ? g_fd : -1; }
pid_t  player_test_pid(void)         { return g_pid; }
int    player_test_spawn_count(void) { return g_spawn_count; }
size_t player_test_tx_queued(void)   { return g_tx_len; }
int    player_test_pending(void)
{
    int n = 0;
    for (int i = 0; i < MAX_REQS; i++) n += (g_req[i].kind != REQ_FREE);
    return n;
}
#endif
