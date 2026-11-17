/**
 * test_msg_cache.c — Unit tests for the per-room message cache.
 *
 * Run: ./test_msg_cache
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

static cJSON *make_event(const char *body)
{
    cJSON *e = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "msgtype", "m.text");
    cJSON_AddStringToObject(content, "body", body);
    cJSON_AddItemToObject(e, "content", content);
    cJSON_AddStringToObject(e, "event_id", body);
    cJSON_AddStringToObject(e, "type", "m.room.message");
    return e;
}

static void test_set_and_get(void)
{
    msg_cache_clear_all();

    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, make_event("hello"));
    cJSON_AddItemToArray(arr, make_event("world"));
    msg_cache_set("!room1:test", arr);

    cJSON *got = msg_cache_get("!room1:test");
    EXPECT(got != NULL, "get returned NULL after set");
    EXPECT(cJSON_GetArraySize(got) == 2, "size mismatch after set");

    cJSON *miss = msg_cache_get("!nope:test");
    EXPECT(miss == NULL, "expected miss returned non-NULL");
}

static void test_overwrite(void)
{
    msg_cache_clear_all();

    cJSON *a1 = cJSON_CreateArray();
    cJSON_AddItemToArray(a1, make_event("a"));
    msg_cache_set("!room:test", a1);

    cJSON *a2 = cJSON_CreateArray();
    cJSON_AddItemToArray(a2, make_event("b1"));
    cJSON_AddItemToArray(a2, make_event("b2"));
    msg_cache_set("!room:test", a2);

    cJSON *got = msg_cache_get("!room:test");
    EXPECT(got != NULL, "get returned NULL after overwrite");
    EXPECT(cJSON_GetArraySize(got) == 2, "overwrite did not replace");
}

static void test_append(void)
{
    msg_cache_clear_all();

    cJSON *e1 = make_event("first");
    msg_cache_append("!room:test", e1);
    cJSON_Delete(e1);

    cJSON *got = msg_cache_get("!room:test");
    EXPECT(got != NULL, "append did not auto-create entry");
    EXPECT(cJSON_GetArraySize(got) == 1, "append count != 1 after first");

    cJSON *e2 = make_event("second");
    msg_cache_append("!room:test", e2);
    cJSON_Delete(e2);

    got = msg_cache_get("!room:test");
    EXPECT(cJSON_GetArraySize(got) == 2, "append count != 2 after second");

    cJSON *last = cJSON_GetArrayItem(got, 1);
    cJSON *body = cJSON_GetObjectItem(cJSON_GetObjectItem(last, "content"), "body");
    EXPECT(body && strcmp(body->valuestring, "second") == 0, "appended event body wrong");
}

static void test_lru_eviction(void)
{
    msg_cache_clear_all();

    /* Fill the cache + 1 to force eviction */
    for (int i = 0; i < MSG_CACHE_MAX_ROOMS; i++) {
        char rid[64];
        snprintf(rid, sizeof(rid), "!room%d:test", i);
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, make_event(rid));
        msg_cache_set(rid, arr);
    }

    /* Touch room0 so it becomes most-recently-used */
    EXPECT(msg_cache_get("!room0:test") != NULL, "room0 missing before evict test");

    /* Insert a new room — should evict room1 (now LRU after touching room0) */
    cJSON *extra = cJSON_CreateArray();
    cJSON_AddItemToArray(extra, make_event("extra"));
    msg_cache_set("!extra:test", extra);

    EXPECT(msg_cache_get("!room0:test") != NULL, "room0 should still be present");
    EXPECT(msg_cache_get("!room1:test") == NULL, "room1 should have been evicted");
    EXPECT(msg_cache_get("!extra:test") != NULL, "extra should be present");
    EXPECT(msg_cache_count() == MSG_CACHE_MAX_ROOMS, "count should equal max");
}

static void test_clear_all(void)
{
    msg_cache_clear_all();

    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, make_event("x"));
    msg_cache_set("!k:test", a);

    EXPECT(msg_cache_count() > 0, "expected entry before clear");
    msg_cache_clear_all();
    EXPECT(msg_cache_count() == 0, "clear_all did not empty cache");
    EXPECT(msg_cache_get("!k:test") == NULL, "entry remained after clear");
}

/* An event_id the room already holds is skipped: history and sync can
 * deliver the same event. */
static void test_append_dedup(void)
{
    msg_cache_clear_all();

    cJSON *a = make_event("$a");
    cJSON *b = make_event("$b");
    EXPECT(msg_cache_append("!room:test", a), "first append returns true");
    EXPECT(msg_cache_append("!room:test", b), "second (other id) returns true");
    EXPECT(!msg_cache_append("!room:test", a), "repeat of an older id returns false");
    EXPECT(!msg_cache_append("!room:test", b), "repeat of the newest id returns false");
    EXPECT(cJSON_GetArraySize(msg_cache_get("!room:test")) == 2, "repeats not stored");
    EXPECT(msg_cache_append("!other:test", a), "same id in another room is new there");
    cJSON_Delete(a);
    cJSON_Delete(b);

    /* No event_id: nothing to compare, always appended. */
    cJSON *anon = cJSON_CreateObject();
    EXPECT(msg_cache_append("!room:test", anon) && msg_cache_append("!room:test", anon),
           "events without an id are always appended");
    cJSON_Delete(anon);
}

/* Past MSG_CACHE_MAX_EVENTS the oldest goes; the newest stay in order. */
static void test_append_cap(void)
{
    msg_cache_clear_all();
    char id[32];
    for (int i = 0; i < MSG_CACHE_MAX_EVENTS + 5; i++) {
        snprintf(id, sizeof(id), "$e%d", i);
        cJSON *e = make_event(id);
        msg_cache_append("!room:test", e);
        cJSON_Delete(e);
    }
    cJSON *got = msg_cache_get("!room:test");
    EXPECT(cJSON_GetArraySize(got) == MSG_CACHE_MAX_EVENTS, "capped at MSG_CACHE_MAX_EVENTS");
    cJSON *first = cJSON_GetObjectItem(cJSON_GetArrayItem(got, 0), "event_id");
    EXPECT(first && strcmp(first->valuestring, "$e5") == 0, "the 5 oldest were dropped");
    cJSON *e = make_event("$e5");
    EXPECT(!msg_cache_append("!room:test", e), "dedup still sees the oldest kept event");
    cJSON_Delete(e);
}

int main(void)
{
    printf("=== msg_cache unit tests ===\n");
    test_set_and_get();
    test_overwrite();
    test_append();
    test_lru_eviction();
    test_clear_all();
    test_append_dedup();
    test_append_cap();
    msg_cache_clear_all();

    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
