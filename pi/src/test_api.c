/**
 * test_api.c — Standalone test binary for the Matrix API client
 *
 * Usage: test_api <homeserver_url> <access_token>
 * Example: test_api http://mock:8008 test_token_123
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matrix_api.h"
#include "cJSON.h"

static const char *TEST_ROOM_ID = "!testroom:localhost";

static int test_get_messages(void)
{
    printf("--- matrix_get_messages (limit=10) ---\n");
    cJSON *msgs = matrix_get_messages(TEST_ROOM_ID, 10);
    if (!msgs) {
        fprintf(stderr, "FAIL: matrix_get_messages returned NULL\n");
        return -1;
    }

    int n = cJSON_GetArraySize(msgs);
    printf("Messages fetched for room '%s' (%d messages)\n", TEST_ROOM_ID, n);
    for (int i = 0; i < n; i++) {
        cJSON *event   = cJSON_GetArrayItem(msgs, i);
        cJSON *evid    = cJSON_GetObjectItem(event, "event_id");
        cJSON *sender  = cJSON_GetObjectItem(event, "sender");
        cJSON *content = cJSON_GetObjectItem(event, "content");
        cJSON *body    = content ? cJSON_GetObjectItem(content, "body") : NULL;
        printf("  [%d] event_id=%-20s sender=%s body=%s\n",
               i,
               evid && evid->valuestring ? evid->valuestring : "?",
               sender && sender->valuestring ? sender->valuestring : "?",
               body && body->valuestring ? body->valuestring : "?");
    }

    cJSON_Delete(msgs);
    return 0;
}

static int test_send_message(void)
{
    printf("--- matrix_send_message ---\n");
    const char *test_msg = "Hello from test_api!";
    int rc = matrix_send_message(TEST_ROOM_ID, test_msg);
    if (rc != 0) {
        fprintf(stderr, "FAIL: matrix_send_message returned %d\n", rc);
        return -1;
    }
    printf("Message sent successfully: '%s'\n", test_msg);
    return 0;
}

static int test_sync(void)
{
    printf("--- matrix_sync (initial) ---\n");
    cJSON *sync = matrix_sync(NULL);
    if (!sync) {
        fprintf(stderr, "FAIL: matrix_sync returned NULL\n");
        return -1;
    }

    cJSON *nb = cJSON_GetObjectItem(sync, "next_batch");
    printf("next_batch: %s\n",
           nb && nb->valuestring ? nb->valuestring : "?");

    cJSON_Delete(sync);
    return 0;
}

static int test_sent_message_appears(void)
{
    printf("--- Verify sent message appears in GET ---\n");
    cJSON *msgs = matrix_get_messages(TEST_ROOM_ID, 20);
    if (!msgs) {
        fprintf(stderr, "FAIL: matrix_get_messages returned NULL after send\n");
        return -1;
    }

    int n = cJSON_GetArraySize(msgs);
    int found = 0;
    for (int i = 0; i < n; i++) {
        cJSON *event   = cJSON_GetArrayItem(msgs, i);
        cJSON *content = cJSON_GetObjectItem(event, "content");
        cJSON *body    = content ? cJSON_GetObjectItem(content, "body") : NULL;
        if (body && body->valuestring &&
            strcmp(body->valuestring, "Hello from test_api!") == 0)
        {
            found = 1;
            break;
        }
    }
    cJSON_Delete(msgs);

    if (!found) {
        fprintf(stderr, "FAIL: sent message not found in subsequent GET\n");
        return -1;
    }
    printf("Sent message appears in subsequent fetch: PASS\n");
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <homeserver_url> <access_token>\n", argv[0]);
        fprintf(stderr, "Example: %s http://mock:8008 test_token_123\n", argv[0]);
        return 1;
    }

    const char *homeserver = argv[1];
    const char *token = argv[2];

    printf("=== Chatbox Matrix API Test ===\n");
    printf("Homeserver: %s\n", homeserver);
    printf("Token: %s\n", token);
    printf("\n");

    matrix_init(homeserver, token);

    int failures = 0;

    if (test_sync() != 0) failures++;
    if (test_get_messages() != 0) failures++;
    if (test_send_message() != 0) failures++;
    if (test_sent_message_appears() != 0) failures++;

    matrix_cleanup();

    printf("\n=== Results: %d failure(s) ===\n", failures);
    return failures == 0 ? 0 : 1;
}
