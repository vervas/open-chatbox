/**
 * test_player.c — Unit tests for the player core (src/player.c,
 * docs/player-spec.md §9, D4, §7.3, §13 T7).
 *
 * Most tests use an in-process fake mpv: the injected spawn function opens
 * a listening unix socket in $TMPDIR, and fake_pump() (called between
 * player_tick()s, same thread) accepts, reads the player's commands,
 * answers them and emits scripted events. The clock is injected too, so
 * backoff and timeouts run in simulated time and everything is
 * deterministic. One test spawns the real test_fake_mpv binary through the
 * default posix_spawn path; one optional smoke test uses the real mpv if
 * it's on PATH (skipped otherwise).
 */

#include "player.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

/* ---- assertions ---- */

static int g_tests = 0, g_failures = 0;

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

/* ---- fake clock ---- */

static uint32_t g_clock = 0;
static uint32_t fake_now(void) { return g_clock; }

/* ---- fake mpv (in-process) ---- */

#define MAX_CMDS 4096

static struct {
    int    lfd, cfd;
    bool   alive;           /* the "process" */
    pid_t  pid;
    bool   listen;          /* spawn creates the socket (false: never does) */
    bool   reading;         /* false: never reads (backpressure)            */
    bool   auto_reply;
    bool   auto_events;     /* start-file/file-loaded or end-file error     */
    bool   fail_spawn;      /* spawn fails with ENOENT                      */
    bool   ao_fail;         /* every loadfile: "audio output init failed"   */
    char   rx[1 << 20];
    size_t rx_len;
    int    kills;
} F;

static cJSON *g_cmds[MAX_CMDS];
static int    g_ncmds = 0;

static char   g_sock[PATH_MAX];
static char   g_tmpdir[PATH_MAX];

/* What the last spawn got. */
static char   g_spawn_argv[16][PATH_MAX];
static int    g_spawn_argc = 0;
static char   g_spawn_xdg[PATH_MAX];
static int    g_spawn_xdg_count = 0;

static void cmds_clear(void)
{
    for (int i = 0; i < g_ncmds; i++) cJSON_Delete(g_cmds[i]);
    g_ncmds = 0;
}

static void fake_close(void)
{
    if (F.cfd >= 0) close(F.cfd);
    if (F.lfd >= 0) close(F.lfd);
    F.cfd = F.lfd = -1;
    unlink(g_sock);
    F.rx_len = 0;
}

static void fake_listen(const char *path)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    unlink(path);
    F.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    fcntl(F.lfd, F_SETFL, O_NONBLOCK);
    if (bind(F.lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(F.lfd, 4) < 0) {
        perror("fake_listen");
        exit(2);
    }
}

static pid_t fake_spawn(const char *path, char *const argv[], char *const envp[], void *ud)
{
    (void)path; (void)ud;
    g_spawn_argc = 0;
    for (int i = 0; argv[i] && i < 16; i++)
        snprintf(g_spawn_argv[g_spawn_argc++], PATH_MAX, "%s", argv[i]);
    g_spawn_xdg[0] = '\0';
    g_spawn_xdg_count = 0;
    for (int i = 0; envp[i]; i++) {
        if (strncmp(envp[i], "XDG_RUNTIME_DIR=", 16) == 0) {
            snprintf(g_spawn_xdg, sizeof(g_spawn_xdg), "%s", envp[i] + 16);
            g_spawn_xdg_count++;
        }
    }
    if (F.fail_spawn) {
        errno = ENOENT;
        return -1;
    }
    fake_close();
    if (F.listen) fake_listen(g_sock);
    F.alive = true;
    F.pid++;
    return F.pid;
}

static bool fake_alive(pid_t h, void *ud)
{
    (void)ud;
    return h == F.pid && F.alive;
}

static void fake_kill(pid_t h, void *ud)
{
    (void)ud;
    if (h == F.pid && F.alive) {
        F.alive = false;
        fake_close();
    }
    F.kills++;
}

static const player_proc_ops_t k_fake_ops = { fake_spawn, fake_alive, fake_kill, NULL };

static void player_tick_n(int n);

/* Write everything to the player, ticking it while the socket is full. */
static void fake_send(const char *s)
{
    size_t len = strlen(s), off = 0;
    for (int guard = 0; off < len && guard < 100000; guard++) {
        ssize_t n = send(F.cfd, s + off, len - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { player_tick_n(1); continue; }
        return;
    }
}

static void fake_respond(cJSON *obj)
{
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(obj, "request_id");
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(obj, "command");
    const cJSON *name = cJSON_GetArrayItem(cmd, 0);
    char buf[256];
    if (F.auto_reply && cJSON_IsNumber(rid)) {
        snprintf(buf, sizeof(buf), "{\"request_id\":%d,\"error\":\"success\",\"data\":null}\n",
                 (int)rid->valuedouble);
        fake_send(buf);
    }
    if (!F.auto_events || !cJSON_IsString(name)) return;
    if (strcmp(name->valuestring, "loadfile") == 0) {
        const cJSON *p = cJSON_GetArrayItem(cmd, 1);
        fake_send("{\"event\":\"start-file\",\"playlist_entry_id\":1}\n");
        if (F.ao_fail)
            fake_send("{\"event\":\"end-file\",\"reason\":\"error\",\"playlist_entry_id\":1,"
                      "\"file_error\":\"audio output initialization failed\"}\n");
        else if (cJSON_IsString(p) && strstr(p->valuestring, "BAD"))
            fake_send("{\"event\":\"end-file\",\"reason\":\"error\",\"playlist_entry_id\":1,"
                      "\"file_error\":\"unrecognized file format\"}\n");
        else
            fake_send("{\"event\":\"file-loaded\"}\n");
    } else if (strcmp(name->valuestring, "stop") == 0) {
        fake_send("{\"event\":\"end-file\",\"reason\":\"stop\",\"playlist_entry_id\":1}\n");
    }
}

static void fake_pump(void)
{
    if (F.lfd >= 0 && F.cfd < 0) {
        int c = accept(F.lfd, NULL, NULL);
        if (c >= 0) {
            fcntl(c, F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            F.cfd = c;
        }
    }
    if (F.cfd < 0 || !F.reading) return;
    for (;;) {
        ssize_t n = recv(F.cfd, F.rx + F.rx_len, sizeof(F.rx) - F.rx_len, 0);
        if (n <= 0) break;
        F.rx_len += (size_t)n;
    }
    size_t start = 0;
    for (size_t i = 0; i < F.rx_len; i++) {
        if (F.rx[i] != '\n') continue;
        cJSON *obj = cJSON_ParseWithLength(F.rx + start, i - start);
        start = i + 1;
        if (!obj) { fprintf(stderr, "fake: bad JSON from player\n"); continue; }
        if (g_ncmds < MAX_CMDS) g_cmds[g_ncmds++] = obj;
        fake_respond(obj);
        if (g_ncmds >= MAX_CMDS) cJSON_Delete(obj);
    }
    memmove(F.rx, F.rx + start, F.rx_len - start);
    F.rx_len -= start;
}

/* The mpv "process" dies: socket gone, waitpid would reap it. */
static void fake_die(void)
{
    F.alive = false;
    fake_close();
}

/* ---- command inspection ---- */

static const cJSON *cmd_arr(int i)
{
    return cJSON_GetObjectItemCaseSensitive(g_cmds[i], "command");
}

static const char *cmd_name(int i)
{
    const cJSON *n = cJSON_GetArrayItem(cmd_arr(i), 0);
    return cJSON_IsString(n) ? n->valuestring : "";
}

static const cJSON *cmd_arg(int i, int k) { return cJSON_GetArrayItem(cmd_arr(i), k); }

static int cmd_id(int i)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(g_cmds[i], "request_id");
    return cJSON_IsNumber(r) ? (int)r->valuedouble : -1;
}

/* Index of the next command `name` at or after `from` (-1 if none). With
 * prop, only set_property of that property. */
static int find_cmd(int from, const char *name, const char *prop)
{
    for (int i = from; i < g_ncmds; i++) {
        if (strcmp(cmd_name(i), name) != 0) continue;
        if (prop) {
            const cJSON *p = cmd_arg(i, 1);
            if (!cJSON_IsString(p) || strcmp(p->valuestring, prop) != 0) continue;
        }
        return i;
    }
    return -1;
}

static int count_cmd(int from, const char *name, const char *prop)
{
    int n = 0;
    for (int i = find_cmd(from, name, prop); i >= 0; i = find_cmd(i + 1, name, prop)) n++;
    return n;
}

static int last_cmd(const char *name, const char *prop)
{
    int r = -1;
    for (int i = find_cmd(0, name, prop); i >= 0; i = find_cmd(i + 1, name, prop)) r = i;
    return r;
}

static const char *last_load_path(void)
{
    int i = last_cmd("loadfile", NULL);
    const cJSON *p = i >= 0 ? cmd_arg(i, 1) : NULL;
    return cJSON_IsString(p) ? p->valuestring : "(none)";
}

/* The last set_property pause value: 1 true, 0 false, -1 none. */
static int last_pause(void)
{
    int i = last_cmd("set_property", "pause");
    if (i < 0) return -1;
    return cJSON_IsTrue(cmd_arg(i, 2)) ? 1 : 0;
}

/* ---- callbacks ---- */

static int g_cb_track, g_cb_state, g_cb_volume, g_cb_pos;

static void cb_track(void *ud)  { (void)ud; g_cb_track++; }
static void cb_state(void *ud)  { (void)ud; g_cb_state++; }
static void cb_volume(void *ud) { (void)ud; g_cb_volume++; }
static int64_t g_cb_pos_value = -1;
static void cb_pos(void *ud)    { (void)ud; g_cb_pos++; g_cb_pos_value = player_position_ms(); }

static void cb_reset(void) { g_cb_track = g_cb_state = g_cb_volume = g_cb_pos = 0; }

/* ---- driving ---- */

#define STEP_MS 10

static void player_tick_n(int n)
{
    for (int i = 0; i < n; i++) player_tick();
}

/* Simulated time: tick, let the fake answer, advance the clock. */
static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += STEP_MS) {
        player_tick();
        fake_pump();
        player_tick();
        g_clock += STEP_MS;
    }
}

static void fake_reset(void)
{
    fake_close();
    cmds_clear();
    F.alive = false;
    F.listen = true;
    F.reading = true;
    F.auto_reply = true;
    F.auto_events = true;
    F.fail_spawn = false;
    F.ao_fail = false;
    F.kills = 0;
}

static void start_player(int vmax, int vdef)
{
    fake_reset();
    g_clock = 1000;
    player_config_t cfg = {
        .mpv_path = "/opt/fake/mpv",
        .socket_path = g_sock,
        .volume_max = vmax,
        .volume_default = vdef,
        .now_ms = fake_now,
        .proc = &k_fake_ops,
    };
    player_init(&cfg);
    player_callbacks_t cb = { cb_track, cb_state, cb_volume, cb_pos, NULL };
    player_set_callbacks(&cb);
    run(50);
    cb_reset();
}

static void stop_player(void)
{
    player_deinit();
    fake_close();
    cmds_clear();
}

static void send_time(double sec)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":%.3f}\n", sec);
    fake_send(buf);
    run(20);
}

static void send_eof(void)
{
    fake_send("{\"event\":\"end-file\",\"reason\":\"eof\",\"playlist_entry_id\":1}\n");
    run(30);
}

/* A queue of n tracks /m/t<i>.mp3 with ids 100+i. */
static char           g_paths[64][PATH_MAX];
static player_track_t g_trk[64];

static void make_queue(int n, const char *prefix)
{
    for (int i = 0; i < n; i++) {
        snprintf(g_paths[i], PATH_MAX, "/m/%st%d.mp3", prefix, i);
        g_trk[i].id = 100 + i;
        g_trk[i].path = g_paths[i];
    }
}

/* =========================================================================
 * Tests
 * ====================================================================== */

static void test_spawn_argv_env(void)
{
    printf("-- spawn argv, XDG_RUNTIME_DIR, socket path\n");
    char saved[PATH_MAX] = "";
    const char *x = getenv("XDG_RUNTIME_DIR");
    bool had = (x != NULL);
    if (had) snprintf(saved, sizeof(saved), "%s", x);

    unsetenv("XDG_RUNTIME_DIR");
    start_player(70, 40);
    EXPECT(player_available(), "connected to the fake");
    EXPECT_INT(g_spawn_argc, 6, "argc");
    EXPECT_STR(g_spawn_argv[0], "/opt/fake/mpv", "argv[0] = mpv path");
    EXPECT_STR(g_spawn_argv[1], "--idle=yes", "argv[1]");
    EXPECT_STR(g_spawn_argv[2], "--no-video", "argv[2]");
    EXPECT_STR(g_spawn_argv[3], "--no-terminal", "argv[3]");
    EXPECT_STR(g_spawn_argv[4], "--audio-display=no", "argv[4]");
    char want[PATH_MAX + 32];
    snprintf(want, sizeof(want), "--input-ipc-server=%s", g_sock);
    EXPECT_STR(g_spawn_argv[5], want, "argv[5]");
    char xdg[64];
    snprintf(xdg, sizeof(xdg), "/run/user/%u", (unsigned)getuid());
    EXPECT_STR(g_spawn_xdg, xdg, "XDG_RUNTIME_DIR added when unset");
    EXPECT_INT(g_spawn_xdg_count, 1, "exactly one XDG_RUNTIME_DIR");
    EXPECT(getenv("XDG_RUNTIME_DIR") == NULL, "the kiosk's own env untouched");
    stop_player();

    setenv("XDG_RUNTIME_DIR", "", 1);
    start_player(70, 40);
    EXPECT_STR(g_spawn_xdg, xdg, "empty XDG_RUNTIME_DIR replaced");
    EXPECT_INT(g_spawn_xdg_count, 1, "exactly one XDG_RUNTIME_DIR (empty one dropped)");
    stop_player();

    setenv("XDG_RUNTIME_DIR", "/custom/xdg", 1);
    start_player(70, 40);
    EXPECT_STR(g_spawn_xdg, "/custom/xdg", "existing XDG_RUNTIME_DIR kept");
    EXPECT_INT(g_spawn_xdg_count, 1, "not duplicated");
    stop_player();

    if (had) setenv("XDG_RUNTIME_DIR", saved, 1);
    else unsetenv("XDG_RUNTIME_DIR");

    /* Socket path: config > $CHATBOX_MPV_SOCKET > default. */
    player_config_t cfg = { .now_ms = fake_now, .proc = &k_fake_ops, .volume_max = 50 };
    unsetenv("CHATBOX_MPV_SOCKET");
    player_init(&cfg);
    EXPECT_STR(player_socket_path(), "/run/chatbox/mpv.sock", "default socket");
    player_deinit();
    setenv("CHATBOX_MPV_SOCKET", "/tmp/dev-mpv.sock", 1);
    player_init(&cfg);
    EXPECT_STR(player_socket_path(), "/tmp/dev-mpv.sock", "socket from env");
    player_deinit();
    cfg.socket_path = "/x/cfg.sock";
    player_init(&cfg);
    EXPECT_STR(player_socket_path(), "/x/cfg.sock", "config beats env");
    player_deinit();
    unsetenv("CHATBOX_MPV_SOCKET");
}

static void test_connect_and_volume_init(void)
{
    printf("-- on connect: observe + volume; initial clamp\n");
    fake_reset();
    g_clock = 5;
    player_config_t cfg = { .socket_path = g_sock, .volume_max = 70, .volume_default = 90,
                            .now_ms = fake_now, .proc = &k_fake_ops };
    player_init(&cfg);
    EXPECT_INT(player_test_spawn_count(), 0, "init spawns nothing");
    EXPECT(!player_available(), "not available before the first tick");
    run(50);
    EXPECT(player_available(), "available after connecting");
    EXPECT_INT(player_volume(), 70, "default 90 clamped to max 70");
    EXPECT(find_cmd(0, "observe_property", NULL) >= 0, "observes");
    const char *want[] = { "time-pos", "duration", "pause", "idle-active" };
    for (int k = 0; k < 4; k++) {
        bool found = false;
        for (int i = find_cmd(0, "observe_property", NULL); i >= 0;
             i = find_cmd(i + 1, "observe_property", NULL)) {
            const cJSON *p = cmd_arg(i, 2);
            if (cJSON_IsString(p) && strcmp(p->valuestring, want[k]) == 0) found = true;
        }
        EXPECT(found, want[k]);
    }
    int v = last_cmd("set_property", "volume");
    EXPECT(v >= 0, "volume sent on connect");
    if (v >= 0) EXPECT_INT(cmd_arg(v, 2)->valueint, 70, "volume value");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 0, "nothing loaded when idle");
    EXPECT_INT(player_state(), PLAYER_IDLE, "idle");
    EXPECT_INT(player_test_pending(), 0, "all requests answered");
    stop_player();

    cfg.volume_default = -5;
    player_init(&cfg);
    EXPECT_INT(player_volume(), 0, "negative default → 0");
    player_deinit();
    cfg.volume_max = 150;
    cfg.volume_default = 120;
    player_init(&cfg);
    EXPECT_INT(player_volume_max(), 100, "max clamped to 100");
    EXPECT_INT(player_volume(), 100, "default clamped to 100");
    player_deinit();
    cfg.volume_max = 0;
    cfg.volume_default = 40;
    player_init(&cfg);
    EXPECT_INT(player_volume(), 0, "max 0 → silent");
    EXPECT_INT(player_volume_step(5), 0, "can't go up from a 0 cap");
    player_deinit();
}

static void test_queue_eof_stop_resume(void)
{
    printf("-- queue: play, eof advance, end → stopped, resume\n");
    start_player(70, 40);
    make_queue(3, "");
    EXPECT(player_play_queue(g_trk, 3, 0), "play_queue");
    EXPECT_INT(g_cb_track, 0, "callbacks only from player_tick");
    run(30);
    EXPECT_STR(last_load_path(), "/m/t0.mp3", "loads the first file");
    int l = last_cmd("loadfile", NULL);
    EXPECT_STR(cmd_arg(l, 2)->valuestring, "replace", "loadfile ... replace");
    int p = last_cmd("set_property", "pause");
    EXPECT(p >= 0 && p < l, "pause set before loadfile");
    EXPECT_INT(last_pause(), 0, "unpaused");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "playing");
    EXPECT_INT(player_current_id(), 100, "current id");
    EXPECT_INT(player_current_index(), 0, "index 0");
    EXPECT_INT(player_queue_len(), 3, "queue len");
    EXPECT_INT(g_cb_track, 1, "track_changed fired");
    EXPECT(g_cb_state >= 1, "state_changed fired");

    send_time(12.5);
    EXPECT_INT(player_position_ms(), 12500, "position from time-pos");
    fake_send("{\"event\":\"property-change\",\"id\":2,\"name\":\"duration\",\"data\":180.25}\n");
    run(20);
    EXPECT_INT(player_duration_ms(), 180250, "duration");

    send_eof();
    EXPECT_INT(player_current_index(), 1, "eof → next");
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "second file loaded");
    EXPECT_INT(player_position_ms(), 0, "from 0:00");
    EXPECT_INT(player_duration_ms(), -1, "duration unknown until mpv says");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "still playing");

    /* A stray "stop" end-file (ours) is ignored. */
    fake_send("{\"event\":\"end-file\",\"reason\":\"stop\"}\n");
    run(30);
    EXPECT_INT(player_current_index(), 1, "reason stop ignored");
    fake_send("{\"event\":\"end-file\",\"reason\":\"quit\"}\n");
    run(30);
    EXPECT_INT(player_current_index(), 1, "reason quit ignored");

    send_eof();
    EXPECT_INT(player_current_index(), 2, "eof → last");
    int mark = g_ncmds;
    send_time(3.0);
    cb_reset();
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "last eof → stopped");
    EXPECT_INT(player_current_index(), 2, "last track stays current");
    EXPECT_INT(player_current_id(), 102, "last id");
    EXPECT_INT(player_position_ms(), 0, "0:00");
    EXPECT(!player_is_playing(), "not playing");
    EXPECT_INT(count_cmd(mark, "stop", NULL), 1, "stop sent to mpv");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 0, "nothing loaded");
    EXPECT_INT(g_cb_track, 0, "no track change on stop");
    EXPECT(g_cb_state >= 1 && g_cb_pos >= 1, "state + position callbacks");

    /* Late time-pos while stopped doesn't move the position. */
    send_time(0.7);
    EXPECT_INT(player_position_ms(), 0, "stopped: time-pos ignored");

    /* Another eof while stopped: nothing. */
    mark = g_ncmds;
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "eof when stopped ignored");
    EXPECT_INT(g_ncmds - mark, 0, "no commands");

    /* Space (toggle) when stopped: nothing, no replay. */
    mark = g_ncmds;
    cb_reset();
    player_toggle();
    run(30);
    EXPECT_INT(player_state(), PLAYER_STOPPED, "toggle when stopped: still stopped");
    EXPECT_INT(g_ncmds - mark, 0, "toggle when stopped: no commands");
    EXPECT_INT(g_cb_state, 0, "toggle when stopped: no state change");

    /* Resume = the last track again from 0:00. */
    mark = g_ncmds;
    player_resume();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "resume from stopped plays");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "reloads");
    EXPECT_STR(last_load_path(), "/m/t2.mp3", "the last track");
    EXPECT_INT(last_pause(), 0, "unpaused");
    EXPECT_INT(player_position_ms(), 0, "from 0:00");
    stop_player();
}

static void test_prev_ipod(void)
{
    printf("-- prev: iPod rule\n");
    start_player(70, 40);
    make_queue(3, "");
    player_play_queue(g_trk, 3, 1);
    run(30);
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "start at index 1");

    /* Exactly 3 s is not "more than 3 s": previous track. */
    send_time(3.0);
    EXPECT_INT(player_position_ms(), 3000, "at 3.000 s");
    int mark = g_ncmds;
    player_prev();
    run(30);
    EXPECT_INT(player_current_index(), 0, "3.000 s → previous track");
    EXPECT_STR(last_load_path(), "/m/t0.mp3", "loads the previous file");
    EXPECT_INT(count_cmd(mark, "seek", NULL), 0, "no seek");
    EXPECT_INT(player_position_ms(), 0, "from 0:00");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "keeps playing");

    /* First track: always restart, even early. */
    send_time(0.5);
    mark = g_ncmds;
    cb_reset();
    player_prev();
    run(30);
    EXPECT_INT(player_current_index(), 0, "first track stays");
    EXPECT_INT(count_cmd(mark, "seek", NULL), 1, "restart = seek");
    int s = last_cmd("seek", NULL);
    EXPECT(cmd_arg(s, 1)->valuedouble == 0.0, "seek to 0");
    EXPECT_STR(cmd_arg(s, 2)->valuestring, "absolute", "absolute seek");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 0, "no reload");
    EXPECT_INT(player_position_ms(), 0, "position 0");
    EXPECT_INT(g_cb_track, 0, "no track change");
    EXPECT(g_cb_pos >= 1, "position callback");

    /* > 3 s on a later track → restart. */
    player_next();
    run(30);
    EXPECT_INT(player_current_index(), 1, "next");
    send_time(3.001);
    mark = g_ncmds;
    player_prev();
    run(30);
    EXPECT_INT(player_current_index(), 1, "3.001 s → restart, same track");
    EXPECT_INT(count_cmd(mark, "seek", NULL), 1, "seek 0");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 0, "no load");

    /* Paused state is kept by prev (both branches). */
    player_toggle();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "paused");
    send_time(1.0);
    mark = g_ncmds;
    player_prev();
    run(30);
    EXPECT_INT(player_current_index(), 0, "previous");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "still paused");
    EXPECT_INT(last_pause(), 1, "loaded paused");
    int pp = find_cmd(mark, "set_property", "pause"), ll = find_cmd(mark, "loadfile", NULL);
    EXPECT(pp >= 0 && pp < ll, "pause before loadfile (no blip of sound)");
    send_time(10.0);
    player_prev();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "restart keeps paused");
    EXPECT_INT(player_position_ms(), 0, "restarted");
    stop_player();
}

static void test_next_and_stopped_edges(void)
{
    printf("-- next, and prev/next while stopped\n");
    start_player(70, 40);
    make_queue(3, "");
    player_play_queue(g_trk, 3, 0);
    run(30);
    player_toggle();            /* paused */
    run(30);
    player_next();
    run(30);
    EXPECT_INT(player_current_index(), 1, "next");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "next keeps paused");
    EXPECT_INT(last_pause(), 1, "loaded paused");
    player_toggle();
    run(30);
    player_next();
    run(30);
    EXPECT_INT(player_current_index(), 2, "next → last");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "keeps playing");
    send_time(42);
    int mark = g_ncmds;
    cb_reset();
    EXPECT(!player_next(), "next on the last track → false");
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "next on last: keeps playing (no stop)");
    EXPECT_INT(player_current_index(), 2, "last stays current");
    EXPECT_INT(player_position_ms(), 42000, "position untouched");
    EXPECT_INT(g_ncmds - mark, 0, "next on last: no commands");
    EXPECT_INT(g_cb_track + g_cb_state, 0, "next on last: no callbacks");
    player_toggle();
    run(30);
    mark = g_ncmds;
    EXPECT(!player_next(), "next on the last track, paused → false");
    run(30);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "next on last: stays paused");
    EXPECT_INT(g_ncmds - mark, 0, "no commands");
    player_toggle();
    run(30);

    /* Only the natural end stops. */
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "eof on last → stopped");
    mark = g_ncmds;
    EXPECT(!player_next(), "next while stopped → false");
    run(30);
    EXPECT_INT(g_ncmds - mark, 0, "next while stopped: nothing");
    EXPECT_INT(player_state(), PLAYER_STOPPED, "still stopped");

    player_prev();
    run(30);
    EXPECT_INT(player_current_index(), 1, "prev while stopped → previous track");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "stopped counts as paused");
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "loaded");
    EXPECT_INT(last_pause(), 1, "paused");
    stop_player();

    /* Single-track queue. */
    start_player(70, 40);
    make_queue(1, "");
    player_play_queue(g_trk, 1, 0);
    run(30);
    EXPECT(!player_next(), "one track: next → false");
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "one track: next → keeps playing");
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "one track: eof → stopped");
    mark = g_ncmds;
    player_prev();
    run(30);
    EXPECT_INT(player_state(), PLAYER_STOPPED, "one track, stopped: prev → still stopped");
    EXPECT_INT(g_ncmds - mark, 0, "no commands");
    player_toggle();
    run(30);
    EXPECT_INT(player_state(), PLAYER_STOPPED, "space when stopped → still stopped");
    EXPECT_INT(g_ncmds - mark, 0, "space when stopped: no commands");
    player_restart();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "restart (Enter) → plays it again");
    stop_player();

    /* Idle: every operation is a no-op. */
    start_player(70, 40);
    mark = g_ncmds;
    player_toggle();
    player_resume();
    player_restart();
    player_prev();
    EXPECT(!player_next(), "idle: next → false");
    run(30);
    EXPECT_INT(player_state(), PLAYER_IDLE, "idle stays idle");
    EXPECT_INT(g_ncmds - mark, 0, "no commands when idle");
    EXPECT_INT(player_current_id(), 0, "no current id");
    EXPECT_INT(player_current_index(), -1, "no index");
    EXPECT(!player_play_queue(g_trk, 0, 0), "empty queue refused");
    EXPECT_INT(player_state(), PLAYER_IDLE, "still idle");
    stop_player();
}

static void test_toggle_and_enter(void)
{
    printf("-- toggle, play_song (Enter on a song), play_queue on the album\n");
    start_player(70, 40);
    make_queue(4, "");
    EXPECT_INT(player_play_song(g_trk, 4, 2), PLAYER_SONG_STARTED, "not current → started");
    run(30);
    EXPECT_INT(player_current_index(), 2, "from that song");
    EXPECT_INT(player_queue_len(), 4, "whole album queued");

    int mark = g_ncmds;
    EXPECT_INT(player_play_song(g_trk, 4, 2), PLAYER_SONG_ALREADY_PLAYING, "current + playing");
    run(30);
    EXPECT_INT(g_ncmds - mark, 0, "Enter on the playing song does nothing");

    player_toggle();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "toggle → paused");
    EXPECT_INT(last_pause(), 1, "pause true");
    send_time(30);
    mark = g_ncmds;
    EXPECT_INT(player_play_song(g_trk, 4, 2), PLAYER_SONG_RESUMED, "current + paused → resumed");
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "playing again");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 0, "not restarted");
    EXPECT_INT(count_cmd(mark, "seek", NULL), 0, "no seek");
    EXPECT_INT(player_position_ms(), 30000, "position kept");

    /* Current song reached via a different queue: Enter resumes, the queue
     * is kept (the artist queue stays). */
    player_toggle();
    run(30);
    player_track_t album[2] = { { 999, "/m/other.mp3" }, { 102, "/m/t2.mp3" } };
    EXPECT_INT(player_play_song(album, 2, 1), PLAYER_SONG_RESUMED, "same id in another list");
    run(30);
    EXPECT_INT(player_queue_len(), 4, "queue kept");

    /* Enter on the album: from track 1 even though a song of it is current. */
    mark = g_ncmds;
    player_play_queue(g_trk, 4, 0);
    run(30);
    EXPECT_INT(player_current_index(), 0, "album → track 1");
    EXPECT_STR(last_load_path(), "/m/t0.mp3", "loaded track 1");
    player_play_queue(g_trk, 4, 0);
    run(30);
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 2, "album Enter always restarts");

    /* Enter on a song in the columns: play_queue from that song, from 0:00,
     * even when it is the current, playing song. */
    player_play_queue(g_trk, 4, 2);
    run(30);
    send_time(40);
    mark = g_ncmds;
    cb_reset();
    player_play_queue(g_trk, 4, 2);
    run(30);
    EXPECT_INT(player_current_index(), 2, "same song");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "song Enter reloads it");
    EXPECT_INT(player_position_ms(), 0, "from 0:00");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "playing");
    EXPECT_INT(last_pause(), 0, "unpaused");

    /* Stopped: → on the current song resumes from 0:00. */
    player_play_queue(g_trk, 4, 3);
    run(30);
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "stopped");
    EXPECT_INT(player_play_song(g_trk, 4, 3), PLAYER_SONG_RESUMED, "stopped → resumed");
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "plays");
    EXPECT_STR(last_load_path(), "/m/t3.mp3", "the last track again");

    /* player_resume: playing → nothing. */
    mark = g_ncmds;
    player_resume();
    run(30);
    EXPECT_INT(g_ncmds - mark, 0, "resume while playing: nothing");
    stop_player();
}

static void test_restart(void)
{
    printf("-- restart (Enter on Now Playing): current track from 0:00, playing\n");
    start_player(70, 40);
    make_queue(3, "");
    player_play_queue(g_trk, 3, 1);
    run(30);
    send_time(40);
    fake_send("{\"event\":\"property-change\",\"id\":2,\"name\":\"duration\",\"data\":180}\n");
    run(20);

    /* Playing, 40 s in. */
    int mark = g_ncmds;
    cb_reset();
    player_restart();
    EXPECT_INT(g_cb_state + g_cb_pos, 0, "callbacks only from player_tick");
    run(30);
    EXPECT_INT(player_current_index(), 1, "same track");
    EXPECT_INT(player_queue_len(), 3, "queue kept");
    EXPECT_INT(player_position_ms(), 0, "from 0:00");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "playing");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "reloaded");
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "the current file");
    EXPECT_INT(last_pause(), 0, "unpaused");
    EXPECT_INT(player_duration_ms(), 180000, "same file: duration kept");
    EXPECT_INT(g_cb_track, 0, "no track change");
    EXPECT(g_cb_pos >= 1, "position callback");

    /* Twice in a row: restarts again (Enter Enter = from the beginning). */
    send_time(2);
    mark = g_ncmds;
    player_restart();
    run(30);
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "second restart reloads again");
    EXPECT_INT(player_position_ms(), 0, "0:00 again");

    /* Paused: restarts and plays. */
    send_time(20);
    player_toggle();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "paused");
    mark = g_ncmds;
    cb_reset();
    player_restart();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "paused → playing");
    EXPECT_INT(player_position_ms(), 0, "from 0:00");
    EXPECT_INT(last_pause(), 0, "unpaused");
    int pp = find_cmd(mark, "set_property", "pause"), ll = find_cmd(mark, "loadfile", NULL);
    EXPECT(pp >= 0 && pp < ll, "pause false set before the loadfile");
    EXPECT(g_cb_state >= 1, "state callback");

    /* Stopped (natural end): the last track again from 0:00. */
    player_next();
    run(30);
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "stopped");
    mark = g_ncmds;
    player_restart();
    run(30);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "stopped → playing");
    EXPECT_INT(player_current_index(), 2, "the last track");
    EXPECT_STR(last_load_path(), "/m/t2.mp3", "loaded");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "one load");
    stop_player();

    /* Audio output failed (paused, reload pending): restart loads it. */
    start_player(70, 40);
    make_queue(2, "");
    F.ao_fail = true;
    player_play_queue(g_trk, 2, 0);
    run(50);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "AO failure → paused");
    F.ao_fail = false;
    mark = g_ncmds;
    player_restart();
    run(50);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "restart after the AO failure plays");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "reloaded");
    EXPECT_INT(count_cmd(mark, "seek", NULL), 0, "from 0:00: no seek");
    stop_player();

    /* Idle: nothing. */
    start_player(70, 40);
    mark = g_ncmds;
    player_restart();
    run(30);
    EXPECT_INT(player_state(), PLAYER_IDLE, "idle: restart does nothing");
    EXPECT_INT(g_ncmds - mark, 0, "no commands");
    stop_player();
}

static void test_volume(void)
{
    printf("-- volume clamp\n");
    start_player(70, 60);
    EXPECT_INT(player_volume(), 60, "initial");
    int mark = g_ncmds;
    EXPECT_INT(player_volume_step(5), 65, "+5");
    EXPECT_INT(player_volume_step(5), 70, "+5 → cap");
    EXPECT_INT(player_volume_step(5), 70, "stays at cap");
    run(30);
    EXPECT_INT(count_cmd(mark, "set_property", "volume"), 2, "sent only on change");
    EXPECT_INT(cmd_arg(last_cmd("set_property", "volume"), 2)->valueint, 70, "70 sent");
    EXPECT_INT(g_cb_volume, 1, "one coalesced volume callback per tick");
    for (int i = 0; i < 20; i++) player_volume_step(-5);
    run(30);
    EXPECT_INT(player_volume(), 0, "down to 0");
    EXPECT_INT(player_volume_step(-5), 0, "stays at 0");
    EXPECT_INT(cmd_arg(last_cmd("set_property", "volume"), 2)->valueint, 0, "0 sent");
    stop_player();

    /* A cap that isn't a multiple of 5. */
    start_player(72, 70);
    EXPECT_INT(player_volume_step(5), 72, "clamped to 72");
    stop_player();

    /* Volume changed while mpv is down is applied on the next connect. */
    start_player(70, 40);
    fake_die();
    run(20);
    player_volume_step(-5);
    player_volume_step(-5);
    cmds_clear();
    run(1000);
    EXPECT(player_available(), "reconnected");
    int v = last_cmd("set_property", "volume");
    EXPECT(v >= 0 && cmd_arg(v, 2)->valueint == 30, "volume 30 restored on connect");
    stop_player();
}

static void test_errors(void)
{
    printf("-- end-file error: skip; all-bad queue stops\n");
    start_player(70, 40);
    player_track_t q[3] = { { 1, "/m/a.mp3" }, { 2, "/m/BAD.mp3" }, { 3, "/m/c.mp3" } };
    player_play_queue(q, 3, 1);
    run(50);
    EXPECT_INT(player_current_index(), 2, "error → skipped to the next");
    EXPECT_STR(last_load_path(), "/m/c.mp3", "next file loaded");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "still playing");
    stop_player();

    start_player(70, 40);
    player_track_t bad[3] = { { 1, "/m/BAD1" }, { 2, "/m/BAD2" }, { 3, "/m/BAD3" } };
    player_play_queue(bad, 3, 0);
    run(100);
    EXPECT_INT(player_state(), PLAYER_STOPPED, "all bad → stopped");
    EXPECT_INT(player_current_index(), 2, "at the last track");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 3, "each tried once");
    run(5000);
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 3, "no retry loop");
    /* Space does nothing when stopped; Enter (restart) tries the last one
     * again, and it stops again. */
    player_toggle();
    run(50);
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 3, "Space when stopped: no retry");
    player_restart();
    run(50);
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 4, "one more try on Enter");
    EXPECT_INT(player_state(), PLAYER_STOPPED, "stopped again");
    stop_player();

    /* Paused skip stays paused. */
    start_player(70, 40);
    player_play_queue(q, 3, 0);
    run(30);
    player_toggle();
    run(30);
    player_next();          /* → BAD, paused */
    run(50);
    EXPECT_INT(player_current_index(), 2, "paused: skipped past the bad file");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "still paused");
    stop_player();

    /* A refused loadfile acts like an error. */
    start_player(70, 40);
    F.auto_reply = false;
    F.auto_events = false;
    make_queue(2, "");
    player_play_queue(g_trk, 2, 0);
    run(30);
    int l = last_cmd("loadfile", NULL);
    char buf[128];
    for (int i = 0; i < g_ncmds; i++) {
        if (i == l) continue;
        snprintf(buf, sizeof(buf), "{\"request_id\":%d,\"error\":\"success\"}\n", cmd_id(i));
        fake_send(buf);
    }
    snprintf(buf, sizeof(buf), "{\"request_id\":%d,\"error\":\"invalid parameter\"}\n", cmd_id(l));
    fake_send(buf);
    run(30);
    EXPECT_INT(player_current_index(), 1, "loadfile error → next track");
    stop_player();
}

static void test_audio_output_error(void)
{
    printf("-- audio output failure: pause and keep the track, don't skip\n");
    start_player(70, 40);
    make_queue(3, "");
    F.ao_fail = true;
    player_play_queue(g_trk, 3, 1);
    run(50);
    EXPECT_INT(player_state(), PLAYER_PAUSED, "AO failure → paused");
    EXPECT_INT(player_current_index(), 1, "index kept");
    EXPECT_INT(player_current_id(), 101, "track kept");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 1, "no skip, no retry by itself");
    run(3000);
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 1, "still no retry loop");
    EXPECT(player_available(), "connection kept");

    /* Space retries; it fails again → paused again, still no skip. */
    player_toggle();
    run(50);
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 2, "play retries the load");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "paused again");
    EXPECT_INT(player_current_index(), 1, "index kept");

    /* Audio comes back: the next play works. */
    F.ao_fail = false;
    int mark = g_ncmds;
    player_toggle();
    run(50);
    EXPECT_INT(player_state(), PLAYER_PLAYING, "plays once audio is back");
    EXPECT_INT(count_cmd(mark, "loadfile", NULL), 1, "reloaded");
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "same track");
    EXPECT_INT(last_pause(), 0, "unpaused after file-loaded");
    EXPECT_INT(count_cmd(mark, "seek", NULL), 0, "from 0:00: no seek");
    stop_player();

    /* During a restore after a respawn: position kept, resumed there. */
    start_player(70, 40);
    make_queue(3, "");
    player_play_queue(g_trk, 3, 2);
    run(30);
    send_time(30);
    fake_die();
    F.ao_fail = true;
    cmds_clear();
    run(700);
    EXPECT(player_available(), "respawned");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 1, "restore tried once");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "AO failure on restore → paused");
    EXPECT_INT(player_current_index(), 2, "index kept");
    EXPECT_INT(player_position_ms(), 30000, "position kept");
    /* Audio back: play reloads and seeks to the kept position. */
    F.ao_fail = false;
    mark = g_ncmds;
    player_toggle();
    run(50);
    int sk = find_cmd(mark, "seek", NULL);
    EXPECT(sk >= 0 && cmd_arg(sk, 1)->valuedouble == 30.0, "play → reload + seek to 30 s");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "playing");
    EXPECT_INT(last_pause(), 0, "unpaused");
    stop_player();

    /* prev (restart) and next while a reload is pending. */
    start_player(70, 40);
    make_queue(3, "");
    F.ao_fail = true;
    player_play_queue(g_trk, 3, 0);
    run(50);
    mark = g_ncmds;
    player_prev();              /* first track: restart */
    run(30);
    EXPECT_INT(count_cmd(mark, "seek", NULL), 0, "no seek into an unloaded file");
    EXPECT_INT(player_position_ms(), 0, "0:00");
    F.ao_fail = false;
    player_next();
    run(50);
    EXPECT_INT(player_current_index(), 1, "next still moves on");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "keeps paused");
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "loads the next");
    stop_player();

    /* A real file error still skips (BAD = "unrecognized file format"). */
    start_player(70, 40);
    player_track_t q[2] = { { 1, "/m/BAD.mp3" }, { 2, "/m/ok.mp3" } };
    player_play_queue(q, 2, 0);
    run(50);
    EXPECT_INT(player_current_index(), 1, "file error skips");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "playing the next");
    stop_player();
}

static void test_request_ids(void)
{
    printf("-- request_id matching, out-of-order replies, stale end-file\n");
    start_player(70, 40);
    F.auto_reply = false;
    F.auto_events = false;
    make_queue(3, "");
    int mark = g_ncmds;
    player_play_queue(g_trk, 3, 0);
    run(30);
    int ip = find_cmd(mark, "set_property", "pause");
    int il = find_cmd(mark, "loadfile", NULL);
    EXPECT(ip >= 0 && il >= 0, "pause + loadfile sent");
    EXPECT(cmd_id(ip) != cmd_id(il) && cmd_id(ip) > 0 && cmd_id(il) > 0, "distinct ids");
    EXPECT_INT(player_test_pending(), 2, "two pending");

    /* An eof before our loadfile's reply is the previous file's. */
    send_eof();
    EXPECT_INT(player_current_index(), 0, "stale eof ignored");
    fake_send("{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":99}\n");
    run(20);
    EXPECT_INT(player_position_ms(), 0, "stale time-pos ignored");

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"request_id\":%d,\"error\":\"success\"}\n", cmd_id(il));
    fake_send(buf);
    fake_send("{\"request_id\":424242,\"error\":\"success\"}\n");     /* unknown */
    fake_send("{\"request_id\":0,\"error\":\"success\"}\n");
    snprintf(buf, sizeof(buf), "{\"request_id\":%d,\"error\":\"success\"}\n", cmd_id(ip));
    fake_send(buf);
    run(30);
    EXPECT_INT(player_test_pending(), 0, "both matched out of order");

    send_time(5.0);
    EXPECT_INT(player_position_ms(), 5000, "time-pos accepted after the reply");
    send_eof();
    EXPECT_INT(player_current_index(), 1, "eof after the reply advances");
    stop_player();
}

static void test_respawn_restore(void)
{
    printf("-- respawn: backoff, restore track/position/pause\n");
    for (int paused = 0; paused <= 1; paused++) {
        start_player(70, 40);
        make_queue(3, "");
        player_play_queue(g_trk, 3, 1);
        run(30);
        player_volume_step(-5);
        if (paused) player_toggle();
        run(30);
        send_time(42.5);
        fake_send("{\"event\":\"property-change\",\"id\":2,\"name\":\"duration\",\"data\":200}\n");
        run(20);
        EXPECT_INT(player_test_spawn_count(), 1, "one spawn so far");

        cb_reset();
        fake_die();
        cmds_clear();
        run(20);
        EXPECT(!player_available(), "death noticed");
        EXPECT(g_cb_state >= 1, "state callback on availability change");
        EXPECT_INT(player_state(), paused ? PLAYER_PAUSED : PLAYER_PLAYING, "intent kept");
        EXPECT_INT(player_position_ms(), 42500, "position kept");
        run(440);
        EXPECT_INT(player_test_spawn_count(), 1, "no respawn before 0.5 s");
        run(60);
        EXPECT_INT(player_test_spawn_count(), 2, "respawned at 0.5 s");
        run(50);
        EXPECT(player_available(), "reconnected");

        int v = find_cmd(0, "set_property", "volume");
        int p = find_cmd(0, "set_property", "pause");
        int l = find_cmd(0, "loadfile", NULL);
        int s = find_cmd(0, "seek", NULL);
        EXPECT(v >= 0 && cmd_arg(v, 2)->valueint == 35, "volume restored");
        EXPECT(p >= 0 && cJSON_IsTrue(cmd_arg(p, 2)) && p < l, "loaded paused first");
        EXPECT_STR(last_load_path(), "/m/t1.mp3", "same track reloaded");
        EXPECT(s > l, "seek after file-loaded");
        if (s >= 0) {
            EXPECT(cmd_arg(s, 1)->valuedouble == 42.5, "seek to the saved position");
            EXPECT_STR(cmd_arg(s, 2)->valuestring, "absolute", "absolute");
        }
        EXPECT_INT(last_pause(), paused ? 1 : 0, "pause state restored");
        EXPECT_INT(count_cmd(0, "set_property", "pause"), paused ? 1 : 2, "pause commands");
        EXPECT_INT(player_state(), paused ? PLAYER_PAUSED : PLAYER_PLAYING, "state kept");
        EXPECT_INT(player_current_index(), 1, "index kept");
        EXPECT_INT(player_position_ms(), 42500, "position kept through the restore");
        EXPECT_INT(player_duration_ms(), 200000, "duration kept");
        send_time(43.0);
        EXPECT_INT(player_position_ms(), 43000, "time-pos flows again");
        player_next();
        run(30);
        EXPECT_INT(player_current_index(), 2, "queue kept");
        stop_player();
    }

    /* Socket EOF while the process is still alive: killed, then respawned. */
    start_player(70, 40);
    make_queue(2, "");
    player_play_queue(g_trk, 2, 0);
    run(30);
    close(F.cfd);
    F.cfd = -1;
    run(20);
    EXPECT(!player_available(), "EOF noticed");
    EXPECT_INT(F.kills, 1, "live process killed");
    run(600);
    EXPECT_INT(player_test_spawn_count(), 2, "respawned");
    EXPECT(player_available(), "reconnected");
    stop_player();

    /* Toggle and prev during the restore window. */
    start_player(70, 40);
    make_queue(2, "");
    player_play_queue(g_trk, 2, 1);
    run(30);
    send_time(50);
    fake_die();
    run(20);
    F.auto_events = false;          /* hold file-loaded back */
    cmds_clear();
    run(600);
    EXPECT(player_available(), "reconnected");
    player_toggle();                /* → paused, applied on file-loaded */
    player_prev();                  /* 50 s in → restart: restore seeks nowhere */
    run(30);
    EXPECT_INT(count_cmd(0, "seek", NULL), 0, "no seek before file-loaded");
    fake_send("{\"event\":\"file-loaded\"}\n");
    run(30);
    EXPECT_INT(count_cmd(0, "seek", NULL), 0, "restart during restore → from 0, no seek");
    EXPECT_INT(last_pause(), 1, "stays paused (toggled during restore)");
    EXPECT_INT(player_state(), PLAYER_PAUSED, "paused");
    EXPECT_INT(player_position_ms(), 0, "0:00");
    stop_player();

    /* Stopped state survives a respawn without loading anything. */
    start_player(70, 40);
    make_queue(1, "");
    player_play_queue(g_trk, 1, 0);
    run(30);
    send_eof();
    EXPECT_INT(player_state(), PLAYER_STOPPED, "stopped");
    fake_die();
    cmds_clear();
    run(700);
    EXPECT(player_available(), "reconnected");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 0, "stopped: nothing reloaded");
    EXPECT_INT(player_state(), PLAYER_STOPPED, "still stopped");
    stop_player();
}

static void test_backoff_and_missing_mpv(void)
{
    printf("-- spawn failure: slow retry, state machine still works\n");
    fake_reset();
    F.fail_spawn = true;
    g_clock = 0;
    player_config_t cfg = { .socket_path = g_sock, .volume_max = 70, .volume_default = 40,
                            .now_ms = fake_now, .proc = &k_fake_ops };
    player_init(&cfg);
    for (uint32_t t = 0; t <= 60000; t += STEP_MS) {
        player_tick();
        g_clock += STEP_MS;
    }
    /* t = 0, 0.5, 1.5, 3.5, then every 5 s: 8.5 … 58.5 → 4 + 11. */
    EXPECT_INT(player_test_spawn_count(), 15, "spawn attempts in 60 s");
    EXPECT(!player_available(), "not available");

    make_queue(3, "");
    EXPECT(player_play_queue(g_trk, 3, 0), "play works without mpv");
    player_tick();
    EXPECT_INT(player_state(), PLAYER_PLAYING, "intent: playing");
    player_next();
    EXPECT_INT(player_current_index(), 1, "next works");
    EXPECT_INT(player_volume_step(5), 45, "volume works");

    /* Real time per tick while mpv is missing: trivial. */
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 10000; i++) player_tick();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    EXPECT(ms < 500, "10000 idle ticks well under 500 ms");
    EXPECT_INT(player_test_spawn_count(), 15, "no spawn without time passing");

    /* mpv appears (installed / PipeWire up): the next attempt connects and
     * applies the state. */
    F.fail_spawn = false;
    run(5100);
    EXPECT_INT(player_test_spawn_count(), 16, "next attempt within 5 s");
    EXPECT(player_available(), "connected");
    EXPECT_STR(last_load_path(), "/m/t1.mp3", "current track loaded on connect");
    int v = last_cmd("set_property", "volume");
    EXPECT(v >= 0 && cmd_arg(v, 2)->valueint == 45, "volume applied");

    /* After a stable 10 s connection the backoff starts over at 0.5 s. */
    run(10500);
    fake_die();
    run(20);
    int before = player_test_spawn_count();
    run(500);
    EXPECT_INT(player_test_spawn_count(), before + 1, "backoff reset after a stable run");

    /* A quick crash right after connecting keeps backing off: 1 s next. */
    run(100);
    fake_die();
    run(20);
    before = player_test_spawn_count();
    run(600);
    EXPECT_INT(player_test_spawn_count(), before, "not at 0.5 s");
    run(500);
    EXPECT_INT(player_test_spawn_count(), before + 1, "at 1 s");
    stop_player();
}

static void test_connect_timeout_and_hang(void)
{
    printf("-- no socket in time; hung mpv; ticks never block\n");
    fake_reset();
    F.listen = false;           /* "mpv" runs but never opens its socket */
    g_clock = 0;
    player_config_t cfg = { .socket_path = g_sock, .volume_max = 70, .volume_default = 40,
                            .now_ms = fake_now, .proc = &k_fake_ops };
    player_init(&cfg);
    run(4900);
    EXPECT_INT(player_test_spawn_count(), 1, "waiting for the socket");
    EXPECT_INT(F.kills, 0, "not killed yet");
    run(200);
    EXPECT_INT(F.kills, 1, "killed after 5 s");
    F.listen = true;
    run(600);
    EXPECT_INT(player_test_spawn_count(), 2, "respawned");
    EXPECT(player_available(), "connected");
    stop_player();

    /* Hung: accepts, reads, never answers. */
    start_player(70, 40);
    F.auto_reply = false;
    F.auto_events = false;
    make_queue(2, "");
    player_play_queue(g_trk, 2, 0);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    run(9000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    EXPECT(ms < 2000, "900 steps against a silent mpv stay fast");
    EXPECT(player_available(), "still connected at 9 s");
    EXPECT_INT(F.kills, 0, "not killed yet");
    run(1100);
    EXPECT_INT(F.kills, 1, "unanswered for 10 s → killed");
    EXPECT(!player_available(), "down");
    F.auto_reply = true;
    F.auto_events = true;
    cmds_clear();
    run(700);
    EXPECT(player_available(), "respawned");
    EXPECT_STR(last_load_path(), "/m/t0.mp3", "track restored");
    stop_player();
}

static void test_lines(void)
{
    printf("-- partial, multiple, oversized and junk lines\n");
    start_player(70, 40);
    make_queue(2, "");
    player_play_queue(g_trk, 2, 0);
    run(30);

    fake_send("{\"event\":\"property-change\",\"id\":1,\"na");
    run(20);
    EXPECT_INT(player_position_ms(), 0, "half a line: nothing yet");
    fake_send("me\":\"time-pos\",\"data\":7.25}\n");
    run(20);
    EXPECT_INT(player_position_ms(), 7250, "reassembled");

    /* Byte by byte. */
    const char *ev = "{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":8}\n";
    for (const char *c = ev; *c; c++) {
        char one[2] = { *c, 0 };
        fake_send(one);
        player_tick();
    }
    run(10);
    EXPECT_INT(player_position_ms(), 8000, "byte by byte");

    /* Many lines in one write. */
    static char many[65536];
    size_t off = 0;
    for (int i = 1; i <= 200; i++)
        off += (size_t)snprintf(many + off, sizeof(many) - off,
                                "{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\","
                                "\"data\":%d}\n", 10 + i);
    fake_send(many);
    run(20);
    EXPECT_INT(player_position_ms(), 210000, "200 lines in one write: last wins");

    /* Oversized line (100 KB), then a good one in the same stream. */
    static char big[110000];
    memset(big, 'x', sizeof(big));
    static const char hdr[] =
        "{\"event\":\"property-change\",\"name\":\"time-pos\",\"data\":1,\"pad\":\"";
    memcpy(big, hdr, sizeof(hdr) - 1);
    big[100000] = '"';
    big[100001] = '}';
    big[100002] = '\n';
    big[100003] = '\0';
    fake_send(big);
    fake_send("{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":300}\n");
    run(30);
    EXPECT(player_available(), "still connected after a huge line");
    EXPECT_INT(player_position_ms(), 300000, "resynced at the next line");

    /* Oversized line split so the newline lands exactly at a buffer edge
     * and a good line follows in the same chunk. */
    static char edge[16384 * 2 + 200];
    memset(edge, 'y', sizeof(edge));
    edge[16384 * 2 - 1] = '\n';
    snprintf(edge + 16384 * 2, 200,
             "{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":301}\n");
    fake_send(edge);
    run(30);
    EXPECT_INT(player_position_ms(), 301000, "newline at the buffer edge");

    /* Junk. */
    fake_send("not json at all\n\n[1,2,3]\n{\"event\":42}\n{\"request_id\":\"x\"}\n");
    fake_send("{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":null}\n");
    fake_send("{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":\"7\"}\n");
    run(30);
    EXPECT(player_available(), "junk ignored, still connected");
    EXPECT_INT(player_position_ms(), 301000, "null / string time-pos ignored");
    stop_player();
}

static void test_write_backpressure(void)
{
    printf("-- write backpressure and ring overflow\n");
    start_player(70, 40);
    static char paths[48][3100];
    player_track_t q[48];
    for (int i = 0; i < 48; i++) {
        memset(paths[i], 'a' + (i % 26), 3000);
        snprintf(paths[i] + 3000, 100, "/%02d.mp3", i);
        paths[i][0] = '/';
        q[i].id = i + 1;
        q[i].path = paths[i];
    }
    player_play_queue(q, 48, 0);
    run(30);
    cmds_clear();

    /* Small kernel buffers on both ends, so the test doesn't depend on the
     * platform's defaults (Linux: ~200 KB, macOS: 8 KB). */
    int small = 4096;
    setsockopt(player_test_fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(F.cfd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    F.reading = false;
    int n = 0;
    while (player_test_tx_queued() == 0 && n < 40) {
        player_next();
        player_tick();
        n++;
    }
    EXPECT(player_test_tx_queued() > 0, "socket full → bytes wait in the ring");
    EXPECT(player_available(), "still connected under backpressure");
    player_next();
    player_next();
    n += 2;
    int want_idx = player_current_index();

    F.reading = true;
    run(100);
    EXPECT_INT(player_test_tx_queued(), 0, "ring drained");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), n, "every loadfile arrived");
    bool order = true;
    int k = 0;
    for (int i = find_cmd(0, "loadfile", NULL); i >= 0; i = find_cmd(i + 1, "loadfile", NULL)) {
        k++;
        if (strcmp(cmd_arg(i, 1)->valuestring, paths[k]) != 0) order = false;
    }
    EXPECT(order, "in order, intact");
    EXPECT_INT(player_current_index(), want_idx, "index as asked");
    EXPECT_INT(player_test_pending(), 0, "all answered");

    /* Overflow: mpv stops reading for good → drop + respawn + restore. */
    F.reading = false;
    int spawns = player_test_spawn_count();
    for (int i = 0; i < 40 && player_available(); i++) {
        if (player_current_index() >= 46) player_prev();
        else player_next();
        player_tick();
    }
    EXPECT(!player_available(), "ring overflow drops the connection");
    EXPECT_INT(F.kills, 1, "hung mpv killed");
    want_idx = player_current_index();
    F.reading = true;
    cmds_clear();
    run(700);
    EXPECT_INT(player_test_spawn_count(), spawns + 1, "respawned");
    EXPECT(player_available(), "reconnected");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), 1, "one reload");
    EXPECT_STR(last_load_path(), paths[want_idx], "the current track");
    stop_player();
}

static void test_weird_names(void)
{
    printf("-- file names round-trip through JSON\n");
    start_player(70, 40);
    const char *names[] = {
        "/srv/music/Artist \"Quoted\"/It's a \"song\".mp3",
        "/srv/music/back\\slash\\dir/a\\\"b.flac",
        "/srv/music/with spaces/  lead and trail  .ogg",
        "/srv/music/Бьорк/Ζωή/日本語 🎵 é.mp3",
        "/srv/music/new\nline/tab\there\r.mp3",
        "/srv/music/ctl\x01\x1f/x.mp3",
        "/srv/music/{\"command\":[\"quit\"]}/].mp3",
    };
    int n = (int)(sizeof(names) / sizeof(names[0]));
    player_track_t q[8];
    for (int i = 0; i < n; i++) { q[i].id = i + 1; q[i].path = names[i]; }
    player_play_queue(q, n, 0);
    run(30);
    for (int i = 0; i < n; i++) {
        EXPECT_STR(last_load_path(), names[i], "path arrives byte-exact");
        EXPECT_STR(player_current_path(), names[i], "current path");
        if (i < n - 1) { player_next(); run(30); }
    }
    EXPECT(player_available(), "no command was split or broken");
    EXPECT_INT(count_cmd(0, "quit", NULL), 0, "no injected command");
    EXPECT_INT(count_cmd(0, "loadfile", NULL), n, "one loadfile each");
    stop_player();
}

static void test_position_coalescing(void)
{
    printf("-- position callbacks coalesced (≤ 4/s)\n");
    start_player(70, 40);
    make_queue(1, "");
    player_play_queue(g_trk, 1, 0);
    run(300);
    cb_reset();
    char buf[128];
    /* 100 updates over 1 s. */
    for (int i = 1; i <= 100; i++) {
        snprintf(buf, sizeof(buf),
                 "{\"event\":\"property-change\",\"id\":1,\"name\":\"time-pos\",\"data\":%.2f}\n",
                 i * 0.01);
        fake_send(buf);
        run(10);
    }
    EXPECT(g_cb_pos >= 3 && g_cb_pos <= 5, "about 4 callbacks for 100 updates");
    EXPECT_INT(player_position_ms(), 1000, "getter always current");
    int n = g_cb_pos;
    run(300);
    EXPECT_INT(g_cb_pos_value, 1000, "trailing update delivered with the last value");
    EXPECT(g_cb_pos <= n + 1, "at most one trailing callback");
    int m = g_cb_pos;
    run(1000);
    EXPECT_INT(g_cb_pos, m, "no updates → no callbacks");
    send_time(1.0);
    run(300);
    EXPECT_INT(g_cb_pos, m, "same value → no callback");
    stop_player();
}

/* ---- real processes ---- */

static char g_self_dir[PATH_MAX];

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int count_in_file(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char line[16384];
    int n = 0;
    while (fgets(line, sizeof(line), f)) n += (strstr(line, needle) != NULL);
    fclose(f);
    return n;
}

static bool wait_for(bool (*cond)(void), int max_ms)
{
    for (int t = 0; t < max_ms; t += 2) {
        player_tick();
        if (cond()) return true;
        sleep_ms(2);
    }
    return false;
}

static bool cond_available(void) { return player_available(); }
static bool cond_unavailable(void) { return !player_available(); }
static bool cond_stopped(void) { return player_state() == PLAYER_STOPPED; }

static void test_real_fake_mpv(void)
{
    printf("-- real spawn of test_fake_mpv (posix_spawn, waitpid, kill -9)\n");
    char bin[PATH_MAX], rec[PATH_MAX], sock[PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/test_fake_mpv", g_self_dir);
    if (access(bin, X_OK) != 0) {
        printf("   SKIP: %s not built\n", bin);
        return;
    }
    snprintf(rec, sizeof(rec), "%s/record.txt", g_tmpdir);
    snprintf(sock, sizeof(sock), "%s/real.sock", g_tmpdir);
    unlink(rec);
    setenv("FAKE_MPV_RECORD", rec, 1);
    char saved[PATH_MAX] = "";
    const char *x = getenv("XDG_RUNTIME_DIR");
    bool had = x != NULL;
    if (had) snprintf(saved, sizeof(saved), "%s", x);
    unsetenv("XDG_RUNTIME_DIR");

    player_config_t cfg = { .mpv_path = bin, .socket_path = sock,
                            .volume_max = 70, .volume_default = 40 };
    player_init(&cfg);
    EXPECT(wait_for(cond_available, 3000), "connected to the spawned fake");
    if (had) setenv("XDG_RUNTIME_DIR", saved, 1);

    char want[PATH_MAX * 2];
    snprintf(want, sizeof(want),
             "spawn [%s] [--idle=yes] [--no-video] [--no-terminal] [--audio-display=no] "
             "[--input-ipc-server=%s]", bin, sock);
    EXPECT_INT(count_in_file(rec, want), 1, "exact argv");
    char xdg[64];
    snprintf(xdg, sizeof(xdg), "xdg /run/user/%u", (unsigned)getuid());
    EXPECT_INT(count_in_file(rec, xdg), 1, "XDG_RUNTIME_DIR in the child");

    player_track_t q[2] = { { 7, "/m/real one.mp3" }, { 8, "/m/real two.mp3" } };
    player_play_queue(q, 2, 1);
    for (int i = 0; i < 100; i++) { player_tick(); sleep_ms(2); }
    EXPECT_INT(count_in_file(rec, "real two.mp3"), 1, "loadfile reached the process");

    pid_t pid = player_test_pid();
    EXPECT(pid > 0, "child pid");
    if (pid > 0) kill(pid, SIGKILL);
    EXPECT(wait_for(cond_unavailable, 2000), "death noticed");
    EXPECT(wait_for(cond_available, 3000), "respawned and reconnected");
    EXPECT(player_test_pid() != pid, "a new process");
    EXPECT_INT(player_test_spawn_count(), 2, "two spawns");
    for (int i = 0; i < 100; i++) { player_tick(); sleep_ms(2); }
    EXPECT_INT(count_in_file(rec, "real two.mp3"), 2, "track reloaded in the new process");
    EXPECT_INT(player_state(), PLAYER_PLAYING, "still playing");

    pid = player_test_pid();
    player_deinit();
    sleep_ms(20);
    EXPECT(kill(pid, 0) != 0, "deinit leaves no process behind");
    unsetenv("FAKE_MPV_RECORD");
    unlink(rec);
    unlink(sock);
}

/* ---- real mpv (optional: skipped when mpv isn't on PATH) ----
 *
 * Part 1 talks to mpv directly over its socket and checks the protocol
 * facts player.c relies on. Part 2 runs the player against real mpv.
 * Both use --ao=null (test-only extra args; the firmware argv is §9's),
 * so they need no sound device. */

typedef struct {
    pid_t  pid;
    int    fd;
    char   buf[1 << 16];
    size_t len;
    cJSON *ev[2048];
    int    n;
} raw_mpv_t;

static raw_mpv_t g_raw;

static bool raw_start(const char *sock)
{
    memset(&g_raw, 0, sizeof(g_raw));
    char ipc[PATH_MAX + 32];
    snprintf(ipc, sizeof(ipc), "--input-ipc-server=%s", sock);
    char *argv[] = { "mpv", "--idle=yes", "--no-video", "--no-terminal",
                     "--audio-display=no", "--ao=null", ipc, NULL };
    extern char **environ;
    if (posix_spawnp(&g_raw.pid, "mpv", NULL, NULL, argv, environ) != 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
    for (int t = 0; t < 500; t++) {
        g_raw.fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(g_raw.fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            fcntl(g_raw.fd, F_SETFL, O_NONBLOCK);
            return true;
        }
        close(g_raw.fd);
        sleep_ms(10);
    }
    return false;
}

static void raw_stop(void)
{
    if (g_raw.fd > 0) close(g_raw.fd);
    if (g_raw.pid > 0) { kill(g_raw.pid, SIGKILL); waitpid(g_raw.pid, NULL, 0); }
    for (int i = 0; i < g_raw.n; i++) cJSON_Delete(g_raw.ev[i]);
    g_raw.n = 0;
}

static void raw_send(const char *line)
{
    size_t len = strlen(line), off = 0;
    while (off < len) {
        ssize_t w = write(g_raw.fd, line + off, len - off);
        if (w > 0) off += (size_t)w;
        else sleep_ms(1);
    }
}

static void raw_read(void)
{
    for (;;) {
        ssize_t r = read(g_raw.fd, g_raw.buf + g_raw.len, sizeof(g_raw.buf) - 1 - g_raw.len);
        if (r <= 0) break;
        g_raw.len += (size_t)r;
    }
    size_t start = 0;
    for (size_t i = 0; i < g_raw.len; i++) {
        if (g_raw.buf[i] != '\n') continue;
        cJSON *o = cJSON_ParseWithLength(g_raw.buf + start, i - start);
        start = i + 1;
        if (o && g_raw.n < 2048) g_raw.ev[g_raw.n++] = o;
        else cJSON_Delete(o);
    }
    memmove(g_raw.buf, g_raw.buf + start, g_raw.len - start);
    g_raw.len -= start;
}

static const char *raw_event(int i)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(g_raw.ev[i], "event");
    return cJSON_IsString(e) ? e->valuestring : NULL;
}

static int raw_reply_id(int i)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(g_raw.ev[i], "request_id");
    return (!raw_event(i) && cJSON_IsNumber(r)) ? (int)r->valuedouble : -1;
}

static const char *raw_str(int i, const char *key)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(g_raw.ev[i], key);
    return cJSON_IsString(r) ? r->valuestring : "";
}

/* First index >= from that is event `name` (or the reply to `rid` when
 * name is NULL); waits up to max_ms. */
static int raw_wait(int from, const char *name, int rid, int max_ms)
{
    for (int t = 0; t <= max_ms; t += 5) {
        raw_read();
        for (int i = from; i < g_raw.n; i++) {
            if (name && raw_event(i) && strcmp(raw_event(i), name) == 0) return i;
            if (!name && raw_reply_id(i) == rid) return i;
        }
        sleep_ms(5);
    }
    return -1;
}

static void raw_cmd(int rid, const char *cmd_json)
{
    char line[PATH_MAX * 2];
    snprintf(line, sizeof(line), "{\"command\":%s,\"request_id\":%d}\n", cmd_json, rid);
    raw_send(line);
}

static void raw_loadfile(int rid, const char *path)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "command");
    cJSON_AddItemToArray(a, cJSON_CreateString("loadfile"));
    cJSON_AddItemToArray(a, cJSON_CreateString(path));
    cJSON_AddItemToArray(a, cJSON_CreateString("replace"));
    cJSON_AddNumberToObject(o, "request_id", rid);
    char *str = cJSON_PrintUnformatted(o);
    raw_send(str);
    raw_send("\n");
    free(str);
    cJSON_Delete(o);
}

static void test_real_mpv_protocol(const char *f5, const char *f1, const char *sock)
{
    printf("-- real mpv protocol facts (raw IPC)\n");
    if (!raw_start(sock)) {
        EXPECT(false, "real mpv: raw start");
        return;
    }
    /* 1. loadfile's reply comes before the new file's start-file, and the
     *    replaced file ends with reason "stop" (never eof/error) after it:
     *    the player's stale-event guard. */
    raw_loadfile(1, f5);
    int fl = raw_wait(0, "file-loaded", 0, 5000);
    EXPECT(fl >= 0, "raw: first file loaded");
    sleep_ms(300);
    raw_read();
    int mark = g_raw.n;
    raw_loadfile(2, f5);
    int fl2 = raw_wait(mark, "file-loaded", 0, 5000);
    int rep = raw_wait(mark, NULL, 2, 0);
    int sf = raw_wait(mark, "start-file", 0, 0);
    int ef = raw_wait(mark, "end-file", 0, 0);
    EXPECT(fl2 >= 0 && rep >= 0 && sf >= 0 && ef >= 0, "raw: replace produced reply + events");
    EXPECT(rep >= 0 && sf >= 0 && rep < sf, "raw: loadfile reply before the new start-file");
    EXPECT(sf >= 0 && fl2 >= 0 && sf < fl2, "raw: start-file before file-loaded");
    if (ef >= 0) {
        EXPECT_STR(raw_str(ef, "reason"), "stop", "raw: replaced file ends with reason stop");
        printf("   replaced file's end-file came %s the loadfile reply\n",
               ef < rep ? "before" : "after");
    }
    for (int i = mark; i < g_raw.n; i++) {
        if (raw_event(i) && strcmp(raw_event(i), "end-file") == 0)
            EXPECT(strcmp(raw_str(i, "reason"), "stop") == 0, "raw: no eof/error on replace");
    }

    /* 2. pause persists across loadfile. */
    raw_cmd(3, "[\"set_property\",\"pause\",true]");
    EXPECT(raw_wait(mark, NULL, 3, 2000) >= 0, "raw: pause reply");
    mark = g_raw.n;
    raw_loadfile(4, f5);
    EXPECT(raw_wait(mark, "file-loaded", 0, 5000) >= 0, "raw: loaded while paused");
    sleep_ms(500);
    raw_read();
    mark = g_raw.n;
    raw_cmd(5, "[\"get_property\",\"pause\"]");
    raw_cmd(6, "[\"get_property\",\"time-pos\"]");
    int r5 = raw_wait(mark, NULL, 5, 2000), r6 = raw_wait(mark, NULL, 6, 2000);
    EXPECT(r5 >= 0 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g_raw.ev[r5], "data")),
           "raw: pause still true after loadfile");
    const cJSON *tp = r6 >= 0 ? cJSON_GetObjectItemCaseSensitive(g_raw.ev[r6], "data") : NULL;
    EXPECT(cJSON_IsNumber(tp) && tp->valuedouble < 0.1, "raw: paused file doesn't advance");

    /* 3. stop → end-file reason stop, then idle. */
    mark = g_raw.n;
    raw_cmd(7, "[\"stop\"]");
    int es = raw_wait(mark, "end-file", 0, 3000);
    EXPECT(es >= 0, "raw: stop → end-file");
    if (es >= 0) EXPECT_STR(raw_str(es, "reason"), "stop", "raw: stop → reason stop");
    EXPECT(raw_wait(mark, "idle", 0, 3000) >= 0, "raw: then idle");

    /* 4. natural end → end-file reason eof. */
    raw_cmd(8, "[\"set_property\",\"pause\",false]");
    mark = g_raw.n;
    raw_loadfile(9, f1);
    int ee = raw_wait(mark, "end-file", 0, 5000);
    EXPECT(ee >= 0, "raw: 1 s file ends");
    if (ee >= 0) EXPECT_STR(raw_str(ee, "reason"), "eof", "raw: reason eof");
    raw_stop();
}

static int g_real_tracks;
static void real_cb_track(void *ud) { (void)ud; g_real_tracks++; }

static bool cond_pos_1500(void) { return player_position_ms() >= 1500; }

static void test_real_mpv_player(const char *f5, const char *f1, const char *f1b,
                                 const char *sock)
{
    printf("-- real mpv: player end to end\n");
    setenv(PLAYER_TEST_EXTRA_ENV, "--ao=null", 1);
    player_config_t cfg = { .socket_path = sock, .volume_max = 10, .volume_default = 0 };
    player_init(&cfg);
    player_callbacks_t cb = { real_cb_track, NULL, NULL, NULL, NULL };
    player_set_callbacks(&cb);
    EXPECT(wait_for(cond_available, 5000), "real: connected");

    /* eof auto-advance across two short files, then stopped. */
    g_real_tracks = 0;
    player_track_t two[2] = { { 1, f1 }, { 2, f1b } };
    player_play_queue(two, 2, 0);
    EXPECT(wait_for(cond_stopped, 8000), "real: two 1 s files play through → stopped");
    EXPECT_INT(player_current_index(), 1, "real: advanced to the second");
    EXPECT_INT(g_real_tracks, 2, "real: two track changes");
    EXPECT(player_duration_ms() > 900 && player_duration_ms() < 1100, "real: duration ~1 s");

    /* Respawn restore, paused: kill -9 at ~1.5 s, same position after. */
    player_track_t one[1] = { { 3, f5 } };
    player_play_queue(one, 1, 0);
    EXPECT(wait_for(cond_pos_1500, 5000), "real: playing past 1.5 s");
    player_toggle();
    for (int i = 0; i < 100; i++) { player_tick(); sleep_ms(2); }
    int64_t saved = player_position_ms();
    pid_t pid = player_test_pid();
    kill(pid, SIGKILL);
    EXPECT(wait_for(cond_unavailable, 2000), "real: death noticed");
    EXPECT(wait_for(cond_available, 4000), "real: respawned");
    for (int i = 0; i < 500; i++) { player_tick(); sleep_ms(2); }
    int64_t after = player_position_ms();
    EXPECT_INT(player_state(), PLAYER_PAUSED, "real: still paused");
    EXPECT(after >= saved - 100 && after <= saved + 100, "real: paused position restored");
    printf("   paused restore: saved %lld ms, after respawn %lld ms\n",
           (long long)saved, (long long)after);

    /* Playing restore: kill while playing, it continues from there. */
    player_toggle();
    for (int i = 0; i < 150; i++) { player_tick(); sleep_ms(2); }
    saved = player_position_ms();
    EXPECT(saved > after, "real: resumed and advancing");
    kill(player_test_pid(), SIGKILL);
    EXPECT(wait_for(cond_unavailable, 2000), "real: death noticed (playing)");
    EXPECT(wait_for(cond_available, 4000), "real: respawned (playing)");
    for (int i = 0; i < 300; i++) { player_tick(); sleep_ms(2); }
    int64_t later = player_position_ms();
    EXPECT_INT(player_state(), PLAYER_PLAYING, "real: still playing");
    EXPECT(later > saved && later < saved + 2000, "real: continued from the saved position");
    printf("   playing restore: saved %lld ms, 0.6 s after respawn %lld ms\n",
           (long long)saved, (long long)later);
    player_deinit();

    /* Audio output failure (pcm output to an unwritable path): no skip. */
    setenv(PLAYER_TEST_EXTRA_ENV, "--ao=pcm --ao-pcm-file=/nonexistent-dir/x.wav", 1);
    player_init(&cfg);
    EXPECT(wait_for(cond_available, 5000), "real: connected (broken audio)");
    player_track_t q2[2] = { { 1, f1 }, { 2, f1b } };
    player_play_queue(q2, 2, 0);
    for (int i = 0; i < 750; i++) { player_tick(); sleep_ms(2); }
    EXPECT_INT(player_state(), PLAYER_PAUSED, "real: AO failure → paused");
    EXPECT_INT(player_current_index(), 0, "real: AO failure doesn't skip");
    player_deinit();
    unsetenv(PLAYER_TEST_EXTRA_ENV);
}

static void test_real_mpv(void)
{
    printf("-- real mpv (optional)\n");
    if (system("command -v mpv >/dev/null 2>&1") != 0) {
        printf("   SKIP: mpv not on PATH\n");
        return;
    }
    char f5[PATH_MAX], f1[PATH_MAX], f1b[PATH_MAX], sock[PATH_MAX], cmd[PATH_MAX * 3];
    snprintf(f5, sizeof(f5), "%s/silent 5s.wav", g_tmpdir);
    snprintf(f1, sizeof(f1), "%s/silent \"1s\".wav", g_tmpdir);
    snprintf(f1b, sizeof(f1b), "%s/silent 1s b.wav", g_tmpdir);
    snprintf(sock, sizeof(sock), "%s/mpv.sock", g_tmpdir);
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 5 '%s' && "
             "ffmpeg -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 1 '%s' && "
             "cp '%s' '%s'", f5, f1, f1, f1b);
    if (system(cmd) != 0) {
        printf("   SKIP: ffmpeg couldn't make the test files\n");
        return;
    }
    char ver[256] = "";
    FILE *v = popen("mpv --version | head -1", "r");
    if (v) { if (!fgets(ver, sizeof(ver), v)) ver[0] = '\0'; pclose(v); }
    printf("   %s", ver);
    test_real_mpv_protocol(f5, f1, sock);
    unlink(sock);
    test_real_mpv_player(f5, f1, f1b, sock);
    unlink(f5);
    unlink(f1);
    unlink(f1b);
    unlink(sock);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);   /* the fake's writes; the player guards its own */
    memset(&F, 0, sizeof(F));
    F.lfd = F.cfd = -1;

    char tmpl[PATH_MAX];
    const char *td = getenv("TMPDIR");
    snprintf(tmpl, sizeof(tmpl), "%s/tplXXXXXX", (td && *td) ? td : "/tmp");
    /* sun_path is ~104 bytes: fall back to /tmp for a long $TMPDIR. */
    if (strlen(tmpl) > 70) snprintf(tmpl, sizeof(tmpl), "/tmp/tplXXXXXX");
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", tmpl);
    snprintf(g_sock, sizeof(g_sock), "%s/mpv.sock", g_tmpdir);

    char self[PATH_MAX];
    snprintf(self, sizeof(self), "%s", argv[0]);
    snprintf(g_self_dir, sizeof(g_self_dir), "%s", dirname(self));
    (void)argc;

    printf("=== player tests ===\n");
    test_spawn_argv_env();
    test_connect_and_volume_init();
    test_queue_eof_stop_resume();
    test_prev_ipod();
    test_next_and_stopped_edges();
    test_toggle_and_enter();
    test_restart();
    test_volume();
    test_errors();
    test_audio_output_error();
    test_request_ids();
    test_respawn_restore();
    test_backoff_and_missing_mpv();
    test_connect_timeout_and_hang();
    test_lines();
    test_write_backpressure();
    test_weird_names();
    test_position_coalescing();
    test_real_fake_mpv();
    test_real_mpv();

    rmdir(g_tmpdir);
    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
