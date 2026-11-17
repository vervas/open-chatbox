/**
 * test_sync_merge.c — Unit tests for folding /sync into the rooms'
 * timelines (sync_merge.c over msg_cache.c).
 *
 * Run: ./test_sync_merge
 */

#include <stdio.h>
#include <string.h>

#include "sync_merge.h"
#include "msg_cache.h"
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

#define ME    "@kiosk:test"
#define MOM   "!mom:test"
#define DAD   "!dad:test"
#define OTHER "!other:test"

/* What the callbacks saw. */
static struct {
    int seeded;
    int events, initial_events, mine, echoes;
    char last_room[64];
    char last_id[64];
} R;

static void reset(void)
{
    memset(&R, 0, sizeof(R));
}

static bool wants(const char *room_id, void *ud)
{
    (void)ud;
    return strcmp(room_id, OTHER) != 0;
}

static void on_seeded(const char *room_id, void *ud)
{
    (void)room_id; (void)ud;
    R.seeded++;
}

static void on_event(const char *room_id, const cJSON *event, bool initial,
                     bool mine, bool local_echo, void *ud)
{
    (void)ud;
    R.events++;
    if (initial) R.initial_events++;
    if (mine) R.mine++;
    if (local_echo) R.echoes++;
    snprintf(R.last_room, sizeof(R.last_room), "%s", room_id);
    const cJSON *id = cJSON_GetObjectItem(event, "event_id");
    snprintf(R.last_id, sizeof(R.last_id), "%s", cJSON_IsString(id) ? id->valuestring : "");
}

static const sync_merge_ops_t OPS = { wants, on_seeded, on_event, NULL };

/* One timeline event. txn: our device's echo (unsigned.transaction_id). */
static cJSON *ev(const char *id, const char *sender, const char *type, const char *txn)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "event_id", id);
    cJSON_AddStringToObject(e, "type", type ? type : "m.room.message");
    cJSON_AddStringToObject(e, "sender", sender);
    cJSON *c = cJSON_AddObjectToObject(e, "content");
    cJSON_AddStringToObject(c, "msgtype", "m.text");
    cJSON_AddStringToObject(c, "body", id);
    if (txn) cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "unsigned"), "transaction_id", txn);
    return e;
}

/* A sync response: rooms.join.<room>.timeline.events = the given events. */
static cJSON *sync_of(const char *room, cJSON *e1, cJSON *e2, cJSON *e3)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "next_batch", "b1");
    cJSON *join = cJSON_AddObjectToObject(cJSON_AddObjectToObject(s, "rooms"), "join");
    cJSON *tl = cJSON_AddObjectToObject(cJSON_AddObjectToObject(join, room), "timeline");
    cJSON *events = cJSON_AddArrayToObject(tl, "events");
    if (e1) cJSON_AddItemToArray(events, e1);
    if (e2) cJSON_AddItemToArray(events, e2);
    if (e3) cJSON_AddItemToArray(events, e3);
    return s;
}

static int cached(const char *room)
{
    cJSON *a = msg_cache_get(room);
    return a ? cJSON_GetArraySize(a) : -1;
}

static void apply(cJSON *sync, bool initial)
{
    sync_merge_apply(sync, initial, ME, &OPS);
    cJSON_Delete(sync);
}

/* The initial sync seeds each room's timeline: exactly its window. */
static void test_initial_seeds(void)
{
    msg_cache_clear_all();
    reset();
    apply(sync_of(MOM, ev("$1", "@mom:test", NULL, NULL), ev("$2", ME, NULL, NULL),
                  ev("$m", "@mom:test", "m.room.member", NULL)), true);
    EXPECT(cached(MOM) == 2, "initial: both messages cached, the member event not");
    EXPECT(R.seeded == 1, "initial: room seeded once");
    EXPECT(R.events == 2 && R.initial_events == 2, "initial: each message reported as initial");
    EXPECT(R.mine == 1 && R.echoes == 0, "initial: own event is mine, no echo flag");

    /* A second initial sync (e.g. token lost) replaces, never doubles. */
    reset();
    apply(sync_of(MOM, ev("$1", "@mom:test", NULL, NULL), ev("$2", ME, NULL, NULL), NULL), true);
    EXPECT(cached(MOM) == 2, "initial again: replaced, not appended");
}

/* Incremental syncs append only what the room doesn't have. */
static void test_incremental_dedup(void)
{
    msg_cache_clear_all();
    apply(sync_of(MOM, ev("$1", "@mom:test", NULL, NULL), ev("$2", "@mom:test", NULL, NULL),
                  NULL), true);
    reset();
    /* Overlap: $2 again (e.g. a limited sync repeating events), $3 new. */
    apply(sync_of(MOM, ev("$2", "@mom:test", NULL, NULL), ev("$3", "@mom:test", NULL, NULL),
                  NULL), false);
    EXPECT(cached(MOM) == 3, "incremental: only the new event added");
    EXPECT(R.events == 1 && strcmp(R.last_id, "$3") == 0, "incremental: only $3 reported");
    EXPECT(R.initial_events == 0, "incremental: not reported as initial");

    reset();
    apply(sync_of(MOM, ev("$1", "@mom:test", NULL, NULL), ev("$3", "@mom:test", NULL, NULL),
                  NULL), false);
    EXPECT(R.events == 0 && cached(MOM) == 3, "incremental: all repeats, nothing reported");
}

/* Own events: our device's echo (transaction_id) vs another device. */
static void test_own_events(void)
{
    msg_cache_clear_all();
    apply(sync_of(DAD, NULL, NULL, NULL), true);
    reset();
    apply(sync_of(DAD, ev("$echo", ME, NULL, "chatbox_1_2"), ev("$phone", ME, NULL, NULL),
                  ev("$dad", "@dad:test", NULL, NULL)), false);
    EXPECT(R.events == 3, "own: all three reported");
    EXPECT(R.mine == 2, "own: both of ours are mine");
    EXPECT(R.echoes == 1, "own: only the one with transaction_id is our echo");
    EXPECT(cached(DAD) == 3, "own: the echo is cached too (a rebuilt panel shows it)");

    /* A token without a device gets no transaction_id back: the echo is
     * recognised by the event_id our send returned, once. */
    reset();
    sync_merge_note_sent("$sent");
    apply(sync_of(DAD, ev("$sent", ME, NULL, NULL), ev("$phone2", ME, NULL, NULL), NULL), false);
    EXPECT(R.mine == 2 && R.echoes == 1, "own: echo matched by the send's event_id");
    EXPECT(cached(DAD) == 5, "own: event_id echo is cached too");
    reset();
    apply(sync_of(DAD, ev("$sent", ME, NULL, NULL), NULL, NULL), false);
    EXPECT(R.events == 0, "own: the same event again is a dup, not reported");

    /* Not knowing who we are: nothing is mine. */
    reset();
    cJSON *s = sync_of(DAD, ev("$x", ME, NULL, "t"), NULL, NULL);
    sync_merge_apply(s, false, "", &OPS);
    cJSON_Delete(s);
    EXPECT(R.mine == 0 && R.echoes == 0, "own: empty user id, nothing is mine");
}

/* Rooms we don't want are left alone; malformed input is ignored. */
static void test_filters(void)
{
    msg_cache_clear_all();
    reset();
    apply(sync_of(OTHER, ev("$o", "@x:test", NULL, NULL), NULL, NULL), true);
    EXPECT(cached(OTHER) == -1 && R.events == 0 && R.seeded == 0, "unwanted room skipped");

    cJSON *s = cJSON_CreateObject();
    sync_merge_apply(s, false, ME, &OPS);
    sync_merge_apply(NULL, true, ME, &OPS);
    sync_merge_apply(s, true, ME, NULL);
    cJSON_Delete(s);
    EXPECT(R.events == 0, "no rooms / NULL sync / NULL ops: nothing happens");

    /* A room with no timeline still seeds (empty). */
    s = cJSON_Parse("{\"rooms\":{\"join\":{\"!mom:test\":{}}}}");
    sync_merge_apply(s, true, ME, &OPS);
    cJSON_Delete(s);
    EXPECT(cached(MOM) == 0, "initial: a room without timeline seeds empty");
}

int main(void)
{
    printf("=== sync_merge unit tests ===\n");
    test_initial_seeds();
    test_incremental_dedup();
    test_own_events();
    test_filters();
    msg_cache_clear_all();

    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
