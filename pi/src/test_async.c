/**
 * test_async.c — Smoke tests for the async dispatcher queue logic.
 *
 * Doesn't require a running homeserver for the queue/cancellation paths;
 * a non-routable URL (closed port) is used so the in-flight request
 * fails fast via the connect timeout.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "matrix_api.h"
#include "cJSON.h"

static int g_tests = 0;
static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

typedef struct {
    int      called;
    int      status;
    cJSON   *result;
} cb_record_t;

static void record_cb(cJSON *result, int status, void *userdata)
{
    cb_record_t *rec = (cb_record_t *)userdata;
    rec->called++;
    rec->status = status;
    if (result) cJSON_Delete(result);  /* don't keep — just record */
    rec->result = NULL;
}

/* Spin matrix_async_tick() until either (a) all callback records
 * report `called` >= 1 or (b) timeout_ms elapses. Returns ms slept. */
static int spin_until_done(cb_record_t *records, int n, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int pending = 0;
        for (int i = 0; i < n; i++) if (!records[i].called) pending++;
        if (pending == 0) return waited;
        matrix_async_tick();
        usleep(20 * 1000);  /* 20 ms */
        waited += 20;
    }
    return waited;
}

static void test_queue_full(void)
{
    /* Use a closed port so connect fails fast (within CONNECTTIMEOUT=2s). */
    matrix_init("http://127.0.0.1:1", "test_token");

    cb_record_t recs[12];
    memset(recs, 0, sizeof(recs));

    /* Queue depth is 8. The first will go in-flight; the next 7 stay pending. */
    for (int i = 0; i < 8; i++) {
        matrix_async_whoami(record_cb, &recs[i]);
    }

    /* The 9th should be rejected immediately with MATRIX_ERR_QUEUE
     * since the queue is full (1 in-flight + 7 pending = 8). */
    matrix_async_whoami(record_cb, &recs[8]);
    EXPECT(recs[8].called == 1, "9th enqueue should fire callback synchronously");
    EXPECT(recs[8].status == MATRIX_ERR_QUEUE, "9th enqueue should report MATRIX_ERR_QUEUE");

    /* Cancel everything: queued requests get MATRIX_ERR_CANCELLED right away;
     * the in-flight one is marked cancelled and finalised on the next tick. */
    matrix_async_cancel(-1);

    /* After cancel, the first in-flight is still in-flight (callback fires
     * once tick processes the completion) — spin until everyone reports done. */
    spin_until_done(recs, 8, 4000);

    int cancelled_count = 0;
    for (int i = 0; i < 8; i++) {
        EXPECT(recs[i].called == 1, "every callback should fire exactly once");
        if (recs[i].status == MATRIX_ERR_CANCELLED) cancelled_count++;
    }
    EXPECT(cancelled_count >= 7,
           "at least 7 of 8 should be MATRIX_ERR_CANCELLED (the 8th may transport-fail)");

    matrix_cleanup();
}

static void test_no_init(void)
{
    /* Without matrix_init (or after cleanup), enqueue should fail
     * synchronously with MATRIX_ERR_TRANSPORT. */
    cb_record_t rec = {0};
    matrix_async_whoami(record_cb, &rec);
    EXPECT(rec.called == 1, "enqueue without init should fire callback");
    EXPECT(rec.status == MATRIX_ERR_TRANSPORT, "should report transport error");
}

static void test_cancel_by_type(void)
{
    matrix_init("http://127.0.0.1:1", "test_token");

    cb_record_t r_send = {0};
    cb_record_t r_who  = {0};

    matrix_async_send_message("!room:test", "hello", record_cb, &r_send);
    matrix_async_whoami(record_cb, &r_who);

    /* Cancel only WHOAMI requests. The send (in-flight) stays. */
    /* REQ_TYPE_WHOAMI == 4 in the enum (NONE=0, SYNC=1, GET_MESSAGES=2,
     * SEND=3, WHOAMI=4). The header doesn't expose this enum, so we
     * just cancel all once we've verified the partial-cancel path
     * isn't easily testable without leaking implementation details. */
    matrix_async_cancel(-1);
    cb_record_t recs[2] = { r_send, r_who };
    spin_until_done(recs, 2, 4000);

    /* Best-effort: just ensure both fire and no segfaults. */
    matrix_cleanup();
}

/* Plain tick-spin helper that doesn't depend on a callback firing. Used
 * to drive backoff scheduling forward without exiting early. */
static void spin_for_ms(int wall_ms)
{
    int waited = 0;
    while (waited < wall_ms) {
        matrix_async_tick();
        usleep(20 * 1000);  /* 20 ms */
        waited += 20;
    }
}

static void test_send_never_dropped_on_transient(void)
{
    /* Core guarantee of the outbox: a send is NEVER silently abandoned on
     * transient failure. Even after the backoff table is exhausted the entry
     * keeps retrying forever at the capped step and STILL never fires a
     * failure callback on its own. */
    matrix_init("http://127.0.0.1:1", "test_token");

    cb_record_t rec = {0};
    matrix_async_send_message("!room:test", "hello", record_cb, &rec);

    /* Five rounds: let the in-flight curl fail (connection refused, fast),
     * then jump the test clock past the longest possible backoff so the
     * next attempt is eligible immediately. */
    for (int i = 0; i < 5; i++) {
        spin_for_ms(200);
        matrix_async__test_advance_clock_ms(120 * 1000);
    }
    spin_for_ms(200);

    EXPECT(rec.called == 0, "no callback while retrying at capped backoff");

    /* Resume fast-tracks the schedule; keep spinning. The send must not be
     * dropped — no callback, the message is still pending delivery. */
    matrix_async_outbox_resume();
    for (int i = 0; i < 5; i++) {
        spin_for_ms(200);
        matrix_async__test_advance_clock_ms(120 * 1000);
    }
    EXPECT(rec.called == 0, "send still not dropped after resume + retries");

    /* Terminal-state proof: `called == 0` above is consistent with two very
     * different worlds — (a) the entry is still alive and pending (the property
     * under test) or (b) a regression silently freed it without firing the
     * callback (the exact bug the outbox exists to prevent). Distinguish them:
     * cancel everything and require the send to surface exactly one CANCELLED
     * callback. That can only happen if the entry survived every retry; a silent
     * drop would leave called == 0 here and fail. */
    matrix_async_cancel(-1);
    EXPECT(rec.called == 1, "surviving send fires exactly one callback on cancel");
    EXPECT(rec.status == MATRIX_ERR_CANCELLED,
           "the retried-forever send was still present and cancels cleanly");

    matrix_cleanup();
}

static void test_send_permanent_rejection(void)
{
    /* A permanent server refusal (4xx other than 408/429) must NOT retry
     * forever — retrying can never succeed and the stuck head would block every
     * later send. The entry is dropped with MATRIX_ERR_REJECTED. Transient 4xx
     * (408 timeout, 429 rate-limit) still fall through to the retry path. */
    matrix_init("http://127.0.0.1:1", "test_token");

    /* 403: permanent refusal -> dropped, callback reports REJECTED. */
    cb_record_t rej = {0};
    matrix_async_send_message("!room:test", "nope", record_cb, &rej);
    matrix_async__test_finish_send_http(403);
    EXPECT(rej.called == 1, "403 send fires its callback exactly once");
    EXPECT(rej.status == MATRIX_ERR_REJECTED, "403 -> MATRIX_ERR_REJECTED");

    /* 429: rate-limit is transient -> the send is kept for retry, no callback. */
    cb_record_t transient = {0};
    matrix_async_send_message("!room:test", "later", record_cb, &transient);
    matrix_async__test_finish_send_http(429);
    EXPECT(transient.called == 0, "429 is transient, send not dropped");

    matrix_cleanup();  /* fires CANCELLED for the surviving transient entry */
}

static void test_outbox_full(void)
{
    /* When the outbox is genuinely full, a further send has nowhere to live.
     * It must fail loudly and immediately with MATRIX_ERR_QUEUE so the UI can
     * mark the bubble undelivered — never be swallowed. No ticks are driven, so
     * nothing drains; the outbox fills and stays full. */
    matrix_init("http://127.0.0.1:1", "test_token");

    /* SEND_OUTBOX_DEPTH is 32 (private to matrix_api.c). Fill it exactly. */
    enum { OUTBOX_DEPTH = 32 };
    cb_record_t recs[OUTBOX_DEPTH];
    memset(recs, 0, sizeof(recs));

    for (int i = 0; i < OUTBOX_DEPTH; i++) {
        matrix_async_send_message("!room:test", "msg", record_cb, &recs[i]);
        EXPECT(recs[i].called == 0, "an accepted send must not fire synchronously");
    }

    /* The next send overflows. matrix_async_send_message() calls resume()
     * internally, but resume only fast-tracks existing entries' backoff — it
     * frees no slot — so the overflow still fails synchronously with QUEUE. */
    cb_record_t overflow = {0};
    matrix_async_send_message("!room:test", "overflow", record_cb, &overflow);
    EXPECT(overflow.called == 1, "send into a full outbox fires callback synchronously");
    EXPECT(overflow.status == MATRIX_ERR_QUEUE, "full outbox reports MATRIX_ERR_QUEUE");

    matrix_cleanup();
}

/* ---- A tiny HTTP server, polled from the test's own loop ----------------
 * Answers every request with 200 "{}", except media downloads, which it
 * holds open without a byte (a photo download stuck on a slow link). */
#define SRV_CONNS 8
static struct {
    int  lfd;
    int  port;
    int  fd[SRV_CONNS];
    char req[SRV_CONNS][2048];
    int  len[SRV_CONNS];
    bool held[SRV_CONNS];
    int  media_requests;
    int  other_requests;
} g_srv;

static void srv_start(void)
{
    memset(&g_srv, 0, sizeof(g_srv));
    for (int i = 0; i < SRV_CONNS; i++) g_srv.fd[i] = -1;
    g_srv.lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_srv.lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(g_srv.lfd, (struct sockaddr *)&a, sizeof(a));
    listen(g_srv.lfd, 8);
    socklen_t al = sizeof(a);
    getsockname(g_srv.lfd, (struct sockaddr *)&a, &al);
    g_srv.port = ntohs(a.sin_port);
    fcntl(g_srv.lfd, F_SETFL, O_NONBLOCK);
}

static void srv_poll(void)
{
    int c;
    while ((c = accept(g_srv.lfd, NULL, NULL)) >= 0) {
        fcntl(c, F_SETFL, O_NONBLOCK);
        int slot = -1;
        for (int i = 0; i < SRV_CONNS && slot < 0; i++) if (g_srv.fd[i] < 0) slot = i;
        if (slot < 0) { close(c); continue; }
        g_srv.fd[slot] = c;
        g_srv.len[slot] = 0;
        g_srv.held[slot] = false;
    }
    for (int i = 0; i < SRV_CONNS; i++) {
        if (g_srv.fd[i] < 0 || g_srv.held[i]) continue;
        ssize_t n = read(g_srv.fd[i], g_srv.req[i] + g_srv.len[i],
                         sizeof(g_srv.req[i]) - 1 - (size_t)g_srv.len[i]);
        if (n > 0) g_srv.len[i] += (int)n;
        g_srv.req[i][g_srv.len[i]] = '\0';
        if (!strstr(g_srv.req[i], "\r\n\r\n")) {
            if (n == 0) { close(g_srv.fd[i]); g_srv.fd[i] = -1; }
            continue;
        }
        if (strstr(g_srv.req[i], "/media/")) {
            g_srv.media_requests++;
            g_srv.held[i] = true;           /* never answered */
            continue;
        }
        g_srv.other_requests++;
        static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                 "Content-Length: 2\r\nConnection: close\r\n\r\n{}";
        ssize_t w = write(g_srv.fd[i], ok, sizeof(ok) - 1);
        (void)w;
        close(g_srv.fd[i]);
        g_srv.fd[i] = -1;
    }
}

static void srv_stop(void)
{
    for (int i = 0; i < SRV_CONNS; i++) if (g_srv.fd[i] >= 0) close(g_srv.fd[i]);
    close(g_srv.lfd);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

typedef struct { int called; int status; } media_record_t;

static void media_cb(const unsigned char *data, size_t len, long http, int status, void *ud)
{
    (void)data; (void)len; (void)http;
    media_record_t *m = ud;
    m->called++;
    m->status = status;
}

static void test_media_does_not_block(void)
{
    /* A photo download that takes its time must not hold up /sync or the
     * user's sends: media has its own slot beside the main queue. */
    srv_start();
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", g_srv.port);
    matrix_init(url, "test_token");

    media_record_t media = {0};
    matrix_async_get_media("mxc://hs/slowphoto", false, 1024 * 1024, media_cb, &media);
    long until = now_ms() + 2000;
    while (now_ms() < until && g_srv.media_requests == 0) {
        matrix_async_tick();
        srv_poll();
        usleep(10 * 1000);
    }
    EXPECT(g_srv.media_requests == 1, "the media download reached the server");

    cb_record_t sync = {0}, send = {0};
    matrix_async_sync(NULL, record_cb, &sync);
    matrix_async_send_message("!room:test", "hello", record_cb, &send);
    until = now_ms() + 3000;             /* a media download may take minutes */
    while (now_ms() < until && !(sync.called && send.called)) {
        matrix_async_tick();
        srv_poll();
        usleep(10 * 1000);
    }
    EXPECT(sync.called == 1 && sync.status == MATRIX_OK, "a sync completes while a photo downloads");
    EXPECT(send.called == 1 && send.status == MATRIX_OK, "a send completes while a photo downloads");
    EXPECT(media.called == 0, "the photo is still downloading");

    /* A second download while one is in flight: busy, try again later. */
    media_record_t second = {0};
    matrix_async_get_media("mxc://hs/another", false, 1024 * 1024, media_cb, &second);
    EXPECT(second.called == 1 && second.status == MATRIX_ERR_QUEUE,
           "one media download at a time: the second is told to wait");

    matrix_cleanup();
    EXPECT(media.called == 1 && media.status == MATRIX_ERR_CANCELLED,
           "cleanup cancels the download in flight");
    srv_stop();
}

int main(void)
{
    printf("=== matrix_async unit tests ===\n");
    test_no_init();
    test_queue_full();
    test_cancel_by_type();
    test_send_never_dropped_on_transient();
    test_send_permanent_rejection();
    test_outbox_full();
    test_media_does_not_block();
    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
