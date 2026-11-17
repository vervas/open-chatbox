/**
 * matrix_api.c — Matrix client-server API client implementation
 */

#include "matrix_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include "cJSON.h"

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static char g_homeserver[512];
static char g_token[512];
static int  g_initialized = 0;
static unsigned int g_txn_counter = 0;

/* Persistent CURL easy handle. Reused across requests so libcurl can keep
 * the underlying TCP connection + TLS session alive between polls.
 * curl_easy_reset() between calls clears request-specific options without
 * dropping the connection cache. */
static CURL  *g_curl  = NULL;
static CURL  *g_media_curl = NULL;  /* the media slot's own handle (async section) */
static CURLM *g_multi = NULL;   /* defined fully in the async section below */

/* -------------------------------------------------------------------------
 * Dynamic response buffer
 * ---------------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t size;
    size_t max;        /* 0 = no limit; else bigger bodies are cut off */
    bool   too_big;
} response_buf_t;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t bytes = size * nmemb;
    response_buf_t *buf = (response_buf_t *)userdata;
    if (buf->max && buf->size + bytes > buf->max) {
        buf->too_big = true;
        return 0;      /* curl aborts with CURLE_WRITE_ERROR */
    }

    char *tmp = realloc(buf->data, buf->size + bytes + 1);
    if (!tmp) {
        fprintf(stderr, "matrix_api: out of memory in write_callback\n");
        return 0;
    }
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}

/* -------------------------------------------------------------------------
 * HTTP helpers
 * ---------------------------------------------------------------------- */

static struct curl_slist *auth_headers(void)
{
    char hdr[600];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", g_token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, hdr);
    return headers;
}

static cJSON *http_get(const char *url)
{
    if (!g_curl) {
        fprintf(stderr, "matrix_api: not initialized\n");
        return NULL;
    }
    curl_easy_reset(g_curl);

    response_buf_t buf = { NULL, 0, 0, false };
    struct curl_slist *headers = auth_headers();

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(g_curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "matrix_api: GET %s failed: %s\n", url, curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        fprintf(stderr, "matrix_api: GET %s returned HTTP %ld\n", url, http_code);
        free(buf.data);
        return NULL;
    }

    cJSON *root = NULL;
    if (buf.data) {
        root = cJSON_Parse(buf.data);
        if (!root)
            fprintf(stderr, "matrix_api: failed to parse JSON from GET %s\n", url);
        free(buf.data);
    }
    return root;
}

static cJSON *http_put_json(const char *url, const char *json_body)
{
    if (!g_curl) {
        fprintf(stderr, "matrix_api: not initialized\n");
        return NULL;
    }
    curl_easy_reset(g_curl);

    response_buf_t buf = { NULL, 0, 0, false };
    struct curl_slist *headers = auth_headers();
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(g_curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "matrix_api: PUT %s failed: %s\n", url, curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        fprintf(stderr, "matrix_api: PUT %s returned HTTP %ld\n", url, http_code);
        free(buf.data);
        return NULL;
    }

    cJSON *root = NULL;
    if (buf.data) {
        root = cJSON_Parse(buf.data);
        if (!root)
            fprintf(stderr, "matrix_api: failed to parse JSON from PUT %s\n", url);
        free(buf.data);
    }
    return root;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void matrix_init(const char *homeserver_url, const char *access_token)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!g_curl) g_curl = curl_easy_init();
    if (!g_media_curl) g_media_curl = curl_easy_init();
    if (!g_curl || !g_media_curl) {
        fprintf(stderr, "matrix_api: curl_easy_init failed\n");
    }

    strncpy(g_homeserver, homeserver_url, sizeof(g_homeserver) - 1);
    g_homeserver[sizeof(g_homeserver) - 1] = '\0';
    strncpy(g_token, access_token, sizeof(g_token) - 1);
    g_token[sizeof(g_token) - 1] = '\0';

    /* Strip trailing slash */
    size_t len = strlen(g_homeserver);
    if (len > 0 && g_homeserver[len - 1] == '/')
        g_homeserver[len - 1] = '\0';

    g_initialized = 1;
    g_txn_counter = (unsigned int)time(NULL);

    matrix_async_init();
}

/* Forward-declared so matrix_cleanup (defined above the async section)
 * can finalise the in-flight slot before tearing down g_multi/g_curl. */
static void async_finalize_in_flight(void);
static void send_outbox_cancel_all(void);

void matrix_cleanup(void)
{
    /* Cancel + drain any in-flight async requests so callbacks fire and
     * the request slots free their userdata before we tear down. Pending
     * slots are finalised inside matrix_async_cancel; the in-flight slot
     * is finalised here because no further matrix_async_tick() will run. */
    matrix_async_cancel(-1);
    send_outbox_cancel_all();
    async_finalize_in_flight();

    if (g_multi) {
        if (g_curl) curl_multi_remove_handle(g_multi, g_curl);
        if (g_media_curl) curl_multi_remove_handle(g_multi, g_media_curl);
        curl_multi_cleanup(g_multi);
        g_multi = NULL;
    }
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
    }
    if (g_media_curl) {
        curl_easy_cleanup(g_media_curl);
        g_media_curl = NULL;
    }
    curl_global_cleanup();
    g_initialized = 0;
}

cJSON *matrix_get_messages(const char *room_id, int limit)
{
    if (!g_initialized) {
        fprintf(stderr, "matrix_api: not initialized\n");
        return NULL;
    }

    /* URL-encode room_id (contains ! and :) */
    if (!g_curl) return NULL;
    char *encoded = curl_easy_escape(g_curl, room_id, 0);
    if (!encoded) return NULL;

    char url[1024];
    snprintf(url, sizeof(url),
             "%s/_matrix/client/v3/rooms/%s/messages?dir=b&limit=%d",
             g_homeserver, encoded, limit);
    curl_free(encoded);

    cJSON *root = http_get(url);
    if (!root) return NULL;

    /* Extract the "chunk" array of events */
    cJSON *chunk = cJSON_GetObjectItem(root, "chunk");
    if (!cJSON_IsArray(chunk)) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *result = cJSON_DetachItemFromObject(root, "chunk");
    cJSON_Delete(root);
    return result;
}

int matrix_send_message(const char *room_id, const char *text)
{
    if (!g_initialized) {
        fprintf(stderr, "matrix_api: not initialized\n");
        return -1;
    }

    /* URL-encode room_id */
    if (!g_curl) return -1;
    char *encoded = curl_easy_escape(g_curl, room_id, 0);
    if (!encoded) return -1;

    /* Unique transaction ID */
    char txn_id[64];
    snprintf(txn_id, sizeof(txn_id), "chatbox_%u_%u",
             g_txn_counter++, (unsigned int)time(NULL));

    char url[1024];
    snprintf(url, sizeof(url),
             "%s/_matrix/client/v3/rooms/%s/send/m.room.message/%s",
             g_homeserver, encoded, txn_id);
    curl_free(encoded);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "msgtype", "m.text");
    cJSON_AddStringToObject(body, "body", text);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!body_str) {
        fprintf(stderr, "matrix_api: failed to serialize JSON body\n");
        return -1;
    }

    cJSON *response = http_put_json(url, body_str);
    free(body_str);

    if (!response) return -1;
    cJSON_Delete(response);
    return 0;
}

cJSON *matrix_sync(const char *since)
{
    if (!g_initialized) {
        fprintf(stderr, "matrix_api: not initialized\n");
        return NULL;
    }

    char url[1024];
    if (since && since[0] != '\0') {
        /* URL-encode the since token */
        if (!g_curl) return NULL;
        char *encoded = curl_easy_escape(g_curl, since, 0);
        if (!encoded) return NULL;

        snprintf(url, sizeof(url),
                 "%s/_matrix/client/v3/sync?since=%s&timeout=0"
                 "&filter={\"room\":{\"timeline\":{\"limit\":50},"
                 "\"state\":{\"types\":[]}},"
                 "\"presence\":{\"types\":[]}}",
                 g_homeserver, encoded);
        curl_free(encoded);
    } else {
        snprintf(url, sizeof(url),
                 "%s/_matrix/client/v3/sync?timeout=0"
                 "&filter={\"room\":{\"timeline\":{\"limit\":50},"
                 "\"state\":{\"types\":[]}},"
                 "\"presence\":{\"types\":[]}}",
                 g_homeserver);
    }

    return http_get(url);
}

cJSON *matrix_whoami(void)
{
    if (!g_initialized) {
        fprintf(stderr, "matrix_api: not initialized\n");
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/account/whoami", g_homeserver);
    return http_get(url);
}

/* ===========================================================================
 * Async dispatcher (Phase B)
 *
 * Single in-flight request at a time, driven by curl_multi. The LVGL thread
 * calls matrix_async_tick() periodically; the dispatcher transitions any
 * completed transfer through request_finish (which fires the callback) and
 * starts the next queued request.
 *
 * Media downloads (photos) don't use that queue: one can take minutes on
 * a slow link, and /sync and the user's sends must keep flowing meanwhile.
 * They have one slot of their own (g_media, its own easy handle g_media_curl)
 * on the same curl_multi, so at most two transfers run at once: the main
 * queue's and a photo. Still one thread; the callbacks fire from
 * matrix_async_tick() as before.
 * ======================================================================== */

#define MATRIX_QUEUE_DEPTH 8

typedef enum {
    REQ_METHOD_GET = 0,
    REQ_METHOD_PUT,
} req_method_t;

typedef enum {
    REQ_TYPE_NONE         = 0,
    REQ_TYPE_SYNC,
    REQ_TYPE_GET_MESSAGES,
    REQ_TYPE_SEND,
    REQ_TYPE_WHOAMI,
    REQ_TYPE_MEDIA,
} req_type_t;

typedef struct {
    bool                 in_use;
    bool                 cancelled;
    req_type_t           type;
    req_method_t         method;
    char                 url[1024];
    char                *body;          /* malloc'd; NULL for GET */
    response_buf_t       resp;
    matrix_async_cb_t    cb;
    matrix_media_cb_t    media_cb;      /* the media slot: instead of cb */
    size_t               max_bytes;     /* the media slot: body limit */
    void                *userdata;
    struct curl_slist   *headers;       /* lifetime: in-flight only */
} async_request_t;

/* Fire a main-queue slot's callback with no result (cancel, transport
 * failure). */
static void request_fail(async_request_t *r, int status)
{
    if (r->cb) r->cb(NULL, status, r->userdata);
}

/* g_multi is declared at file scope above so matrix_cleanup can see it. */
static async_request_t    g_queue[MATRIX_QUEUE_DEPTH];
static int                g_queue_head = 0;
static int                g_queue_tail = 0;
static int                g_queue_size = 0;
static int                g_in_flight  = -1;   /* index into g_queue */

/* The media slot (see the section comment): in_use from
 * matrix_async_get_media until its callback; g_media_started once its
 * transfer is on g_multi. */
static async_request_t    g_media;
static bool               g_media_started = false;

/* HTTP status of the most-recently-completed request. request_complete sets
 * it just before firing the slot's callback; because callbacks run
 * synchronously on the single main thread, a completion handler (notably
 * send_outbox_done) can read it to classify the failure. 0 = no HTTP status
 * (transport/parse error). */
static long               g_last_http_code = 0;

/* Outgoing messages drain through a dedicated ring instead of g_queue so a
 * burst of sync/history/whoami traffic cannot evict a send and silently lose
 * the user's message. One in-flight at a time matches Matrix's sequential
 * txn_id-per-device expectation; full main queue is a backpressure signal,
 * not a send failure, so we retry on the next tick.
 *
 * Retry policy: each transient failure (transport, HTTP error) advances the
 * entry through SEND_BACKOFF_MS and it retries FOREVER. Once the table is
 * exhausted the backoff is clamped at the last step (60s) and the entry keeps
 * retrying at that cap until it succeeds — just like a normal chat client. A
 * send is NEVER abandoned for a *transient* failure: the whole point of this
 * outbox is that the user's message is not silently lost. There is no terminal
 * paused state; matrix_async_outbox_resume() only fast-tracks a retry.
 *
 * The one exception is a permanent server rejection (HTTP 4xx other than 408
 * timeout / 429 rate-limit): retrying such a send can never succeed, and a
 * stuck head entry would block every later send behind it, so the entry is
 * dropped (callback fired with MATRIX_ERR_REJECTED) and the head advances.
 *
 * Ordering tradeoff (was: "stale-on-inbound", now removed): retrying a send
 * after a long outage can land it out-of-order on the peer's timeline. Matrix
 * stamps origin_server_ts at receive time for a regular user token — the `ts`
 * override that would let us backdate the event is appservice-only, which we
 * are not. We deliberately prefer "never lose a message" over strict ordering
 * on this kiosk: a delayed-but-delivered text beats a dropped one for kids.
 * Do NOT re-introduce a global drop to "fix" ordering. If ordering ever needs
 * addressing, do it locally — e.g. a per-room max-age cutoff that only skips
 * truly ancient sends, or a client-side ordering hint — never by discarding
 * the outbox wholesale. */
#define SEND_OUTBOX_DEPTH 32
static const uint32_t SEND_BACKOFF_MS[] = { 2000, 8000, 20000, 40000, 60000 };
#define SEND_BACKOFF_STEPS ((int)(sizeof(SEND_BACKOFF_MS) / sizeof(SEND_BACKOFF_MS[0])))

typedef struct {
    bool              in_use;
    char              url[1024];
    char             *body;        /* malloc'd; outbox owns it until drain */
    matrix_async_cb_t cb;
    void             *userdata;
    uint8_t           attempts;
    uint64_t          next_retry_ms;     /* monotonic_ms() threshold */
} send_outbox_entry_t;

static send_outbox_entry_t g_send_outbox[SEND_OUTBOX_DEPTH];
static int      g_send_outbox_head = 0;
static int      g_send_outbox_tail = 0;
static int      g_send_outbox_size = 0;
static bool     g_send_in_flight   = false;
static int64_t  g_clock_offset_ms  = 0;  /* test hook */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return now + (uint64_t)g_clock_offset_ms;
}

void matrix_async__test_advance_clock_ms(int64_t delta_ms)
{
    g_clock_offset_ms += delta_ms;
}

static int  try_enqueue_request(req_type_t type, req_method_t method,
                                const char *url, const char *body,
                                matrix_async_cb_t cb, void *userdata);
static void send_outbox_done(cJSON *result, int status, void *userdata);

void matrix_async__test_finish_send_http(long http_code)
{
    /* Simulate the head-of-outbox send completing with `http_code`, driving
     * the real send_outbox_done() completion path (200 success, permanent 4xx
     * rejection, or transient backoff). Mirrors what request_complete would set
     * up for a live transfer: it stashes the HTTP status in g_last_http_code and
     * classifies non-200 as MATRIX_ERR_HTTP before calling the slot's callback.
     * Lets the rejection classifier be exercised without a live server. */
    if (g_send_outbox_size == 0) return;
    send_outbox_entry_t *e = &g_send_outbox[g_send_outbox_head];
    if (!e->in_use) return;
    g_last_http_code = http_code;
    int status = (http_code == 200) ? MATRIX_OK : MATRIX_ERR_HTTP;
    g_send_in_flight = true;   /* send_outbox_done clears it, as a real completion would */
    send_outbox_done(NULL, status, e);
}

static int request_alloc_slot(void)
{
    if (g_queue_size >= MATRIX_QUEUE_DEPTH) return -1;
    int idx = g_queue_tail;
    g_queue_tail = (g_queue_tail + 1) % MATRIX_QUEUE_DEPTH;
    g_queue_size++;
    memset(&g_queue[idx], 0, sizeof(g_queue[idx]));
    g_queue[idx].in_use = true;
    return idx;
}

static int request_next_pending(void)
{
    /* Find the oldest in_use slot that isn't currently in flight. */
    if (g_queue_size == 0) return -1;
    for (int n = 0; n < g_queue_size; n++) {
        int idx = (g_queue_head + n) % MATRIX_QUEUE_DEPTH;
        if (g_queue[idx].in_use && idx != g_in_flight) return idx;
    }
    return -1;
}

static void request_release(int idx)
{
    async_request_t *r = &g_queue[idx];
    free(r->body);
    free(r->resp.data);
    if (r->headers) curl_slist_free_all(r->headers);
    memset(r, 0, sizeof(*r));
    /* Compact the queue head if this was the head slot. */
    while (g_queue_size > 0 && !g_queue[g_queue_head].in_use) {
        g_queue_head = (g_queue_head + 1) % MATRIX_QUEUE_DEPTH;
        g_queue_size--;
    }
}

/* Set up easy handle `h` for request r and put it on g_multi. False when
 * that failed (r's callback not fired yet). */
static bool request_add(CURL *h, async_request_t *r)
{
    if (!h || !g_multi) return false;

    curl_easy_reset(h);
    r->headers = auth_headers();

    curl_easy_setopt(h, CURLOPT_URL, r->url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &r->resp);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 2L);
    /* 20s is intentional: the async dispatcher means a slow response no
     * longer blocks the UI thread, so we want headroom for slow VPN or
     * mobile links. Do not lower without restoring blocking-call protection. */
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    if (r->type == REQ_TYPE_MEDIA) {
        /* A photo can take longer than an API call: give up only when the
         * transfer stalls (under 1 KB/s for 20 s) or after 5 minutes. */
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 20L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
        r->resp.max = r->max_bytes;
        if (r->max_bytes)
            curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)r->max_bytes);
    }

    if (r->method == REQ_METHOD_PUT) {
        r->headers = curl_slist_append(r->headers, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, r->body ? r->body : "");
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, r->headers);

    CURLMcode mc = curl_multi_add_handle(g_multi, h);
    if (mc != CURLM_OK) {
        fprintf(stderr, "matrix_async: curl_multi_add_handle: %s\n",
                curl_multi_strerror(mc));
        return false;
    }
    return true;
}

static void request_start(int idx)
{
    async_request_t *r = &g_queue[idx];
    if (!request_add(g_curl, r)) {
        request_fail(r, MATRIX_ERR_TRANSPORT);
        request_release(idx);
        return;
    }
    g_in_flight = idx;
}

/* Free the media slot's resources and mark it free. */
static void media_release(void)
{
    free(g_media.body);
    free(g_media.resp.data);
    if (g_media.headers) curl_slist_free_all(g_media.headers);
    memset(&g_media, 0, sizeof(g_media));
    g_media_started = false;
}

static void media_start(void)
{
    if (!request_add(g_media_curl, &g_media)) {
        matrix_media_cb_t cb = g_media.media_cb;
        void *ud = g_media.userdata;
        media_release();
        if (cb) cb(NULL, 0, 0, MATRIX_ERR_TRANSPORT, ud);
        return;
    }
    g_media_started = true;
}

/* The media transfer finished (or was cancelled): its callback, then the
 * slot is free. The slot is released before the callback so the callback
 * may start the next download. */
static void media_complete(CURLcode result)
{
    async_request_t *r = &g_media;
    long http_code = 0;
    if (g_media_curl) curl_easy_getinfo(g_media_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (g_media_curl && g_multi) curl_multi_remove_handle(g_multi, g_media_curl);

    int st = MATRIX_OK;
    if (r->cancelled) st = MATRIX_ERR_CANCELLED;
    else if (r->resp.too_big || result == CURLE_FILESIZE_EXCEEDED) st = MATRIX_ERR_TOO_LARGE;
    else if (result != CURLE_OK) st = MATRIX_ERR_TRANSPORT;
    else if (http_code != 200) st = MATRIX_ERR_HTTP;
    if (st != MATRIX_OK && st != MATRIX_ERR_CANCELLED)
        fprintf(stderr, "matrix_async: media %s -> %s (HTTP %ld)\n", r->url,
                st == MATRIX_ERR_TOO_LARGE ? "too large" : curl_easy_strerror(result),
                http_code);

    /* Take the body and the callback out, free the slot, then call. */
    matrix_media_cb_t cb = r->media_cb;
    void *ud = r->userdata;
    char *data = r->resp.data;
    size_t size = r->resp.size;
    r->resp.data = NULL;
    media_release();
    if (cb) {
        bool ok = st == MATRIX_OK;
        cb(ok ? (const unsigned char *)(data ? data : "") : NULL, ok ? size : 0,
           http_code, st, ud);
    }
    free(data);
}

/* Run the easy handle's response through cJSON parse + dispatch the
 * type-specific shape to the callback. */
static void request_complete(int idx, CURLcode result)
{
    async_request_t *r = &g_queue[idx];
    long http_code = 0;
    if (g_curl) curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (g_curl && g_multi) curl_multi_remove_handle(g_multi, g_curl);
    g_in_flight = -1;
    g_last_http_code = http_code;

    int status = MATRIX_OK;
    cJSON *parsed = NULL;

    if (r->cancelled) {
        status = MATRIX_ERR_CANCELLED;
    } else if (result != CURLE_OK) {
        fprintf(stderr, "matrix_async: %s -> %s\n", r->url, curl_easy_strerror(result));
        status = MATRIX_ERR_TRANSPORT;
    } else if (http_code != 200) {
        fprintf(stderr, "matrix_async: %s -> HTTP %ld\n", r->url, http_code);
        status = MATRIX_ERR_HTTP;
    } else if (r->resp.data) {
        parsed = cJSON_Parse(r->resp.data);
        if (!parsed) {
            fprintf(stderr, "matrix_async: failed to parse JSON from %s\n", r->url);
            status = MATRIX_ERR_PARSE;
        }
    }

    /* Type-specific shaping (mirror the sync implementations). */
    cJSON *to_caller = parsed;
    if (status == MATRIX_OK && parsed && r->type == REQ_TYPE_GET_MESSAGES) {
        cJSON *chunk = cJSON_GetObjectItem(parsed, "chunk");
        if (cJSON_IsArray(chunk)) {
            to_caller = cJSON_DetachItemFromObject(parsed, "chunk");
        } else {
            status = MATRIX_ERR_PARSE;
            to_caller = NULL;
        }
        cJSON_Delete(parsed);
        parsed = NULL;
    }

    if (r->cb) r->cb(to_caller, status, r->userdata);
    else if (to_caller) cJSON_Delete(to_caller);

    request_release(idx);
}

void matrix_async_init(void)
{
    if (g_multi) return;
    g_multi = curl_multi_init();
    if (!g_multi) {
        fprintf(stderr, "matrix_async: curl_multi_init failed\n");
    }
    memset(g_queue, 0, sizeof(g_queue));
    g_queue_head = g_queue_tail = g_queue_size = 0;
    g_in_flight  = -1;
    memset(&g_media, 0, sizeof(g_media));
    g_media_started = false;
    memset(g_send_outbox, 0, sizeof(g_send_outbox));
    g_send_outbox_head = g_send_outbox_tail = g_send_outbox_size = 0;
    g_send_in_flight = false;
}

void matrix_async_tick(void)
{
    if (!g_multi) return;

    int still_running = 0;
    curl_multi_perform(g_multi, &still_running);

    /* Drain completed transfers. */
    int n_msgs = 0;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(g_multi, &n_msgs)) != NULL) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURLcode result = msg->data.result;
        if (msg->easy_handle == g_media_curl) {
            if (g_media_started) media_complete(result);
        } else if (g_in_flight >= 0) {
            request_complete(g_in_flight, result);
        }
    }

    /* Start the next pending request, if any. */
    if (g_in_flight < 0) {
        int next = request_next_pending();
        if (next >= 0) request_start(next);
    }
    if (g_media.in_use && !g_media_started) media_start();

    /* Drain the send outbox: at most one outstanding send at a time so Matrix
     * sees txn_ids in order. A full main queue is not a failure — the entry
     * stays put and we try again on the next tick. */
    if (!g_send_in_flight && g_send_outbox_size > 0) {
        send_outbox_entry_t *e = &g_send_outbox[g_send_outbox_head];
        if (e->in_use) {
            if (monotonic_ms() >= e->next_retry_ms) {
                int idx = try_enqueue_request(REQ_TYPE_SEND, REQ_METHOD_PUT,
                                              e->url, e->body, send_outbox_done, e);
                if (idx >= 0) g_send_in_flight = true;
            }
        }
    }
}

static void send_outbox_cancel_all(void)
{
    for (int n = 0; n < SEND_OUTBOX_DEPTH; n++) {
        send_outbox_entry_t *e = &g_send_outbox[n];
        if (!e->in_use) continue;
        if (e->cb) e->cb(NULL, MATRIX_ERR_CANCELLED, e->userdata);
        free(e->body);
        memset(e, 0, sizeof(*e));
    }
    g_send_outbox_head = 0;
    g_send_outbox_tail = 0;
    g_send_outbox_size = 0;
    g_send_in_flight   = false;
}

void matrix_async_cancel(int type)
{
    for (int n = 0; n < MATRIX_QUEUE_DEPTH; n++) {
        async_request_t *r = &g_queue[n];
        if (!r->in_use) continue;
        if (type != -1 && (int)r->type != type) continue;
        r->cancelled = true;
        /* Pending requests that haven't started yet: fire callback now and free. */
        if ((int)n != g_in_flight) {
            request_fail(r, MATRIX_ERR_CANCELLED);
            request_release(n);
        }
        /* In-flight: it'll be marked cancelled and finalised when the
         * tick observes completion. We could curl_multi_remove_handle
         * eagerly, but waiting one tick keeps cleanup logic in one place. */
    }
    if ((type == -1 || type == REQ_TYPE_MEDIA) && g_media.in_use) {
        g_media.cancelled = true;
        /* Not started yet: done now. In flight: finalised by the tick. */
        if (!g_media_started) {
            matrix_media_cb_t cb = g_media.media_cb;
            void *ud = g_media.userdata;
            media_release();
            if (cb) cb(NULL, 0, 0, MATRIX_ERR_CANCELLED, ud);
        }
    }
    if (type == -1 || type == REQ_TYPE_SEND) {
        send_outbox_cancel_all();
    }
}

/* Force-finalise the in-flight slot without waiting for a tick. Used by
 * matrix_cleanup, where no further ticks will run. */
static void async_finalize_in_flight(void)
{
    if (g_media_started) {
        g_media.cancelled = true;
        media_complete(CURLE_ABORTED_BY_CALLBACK);
    }
    if (g_in_flight < 0) return;
    int idx = g_in_flight;
    async_request_t *r = &g_queue[idx];
    if (g_multi && g_curl) curl_multi_remove_handle(g_multi, g_curl);
    request_fail(r, MATRIX_ERR_CANCELLED);
    g_in_flight = -1;
    request_release(idx);
}

/* -------------------------------------------------------------------------
 * Per-method enqueue helpers
 * ---------------------------------------------------------------------- */

static int enqueue_request(req_type_t type, req_method_t method,
                           const char *url, const char *body,
                           matrix_async_cb_t cb, void *userdata)
{
    if (!g_multi) matrix_async_init();
    if (strlen(url) >= sizeof(g_queue[0].url)) {
        fprintf(stderr, "matrix_async: url exceeds %zu bytes, dropping\n",
                sizeof(g_queue[0].url));
        if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata);
        return -1;
    }
    int idx = request_alloc_slot();
    if (idx < 0) {
        fprintf(stderr, "matrix_async: queue full, dropping %s\n", url);
        if (cb) cb(NULL, MATRIX_ERR_QUEUE, userdata);
        return -1;
    }
    async_request_t *r = &g_queue[idx];
    r->type     = type;
    r->method   = method;
    strncpy(r->url, url, sizeof(r->url) - 1);
    r->url[sizeof(r->url) - 1] = '\0';
    r->body     = body ? strdup(body) : NULL;
    r->cb       = cb;
    r->userdata = userdata;
    return idx;
}

/* Same as enqueue_request, but a full main queue or oversized URL is signalled
 * via -1 instead of firing the callback. The send outbox uses this so a
 * transient g_queue full state is treated as backpressure (retry next tick),
 * not a permanent send failure that surfaces to the user. */
static int try_enqueue_request(req_type_t type, req_method_t method,
                               const char *url, const char *body,
                               matrix_async_cb_t cb, void *userdata)
{
    if (!g_multi) matrix_async_init();
    if (strlen(url) >= sizeof(g_queue[0].url)) {
        return -1;
    }
    int idx = request_alloc_slot();
    if (idx < 0) {
        return -1;
    }
    async_request_t *r = &g_queue[idx];
    r->type     = type;
    r->method   = method;
    strncpy(r->url, url, sizeof(r->url) - 1);
    r->url[sizeof(r->url) - 1] = '\0';
    r->body     = body ? strdup(body) : NULL;
    r->cb       = cb;
    r->userdata = userdata;
    return idx;
}

static void send_outbox_done(cJSON *result, int status, void *userdata)
{
    send_outbox_entry_t *e = (send_outbox_entry_t *)userdata;
    g_send_in_flight = false;

    if (status == MATRIX_OK) {
        if (e->cb) e->cb(result, status, e->userdata);
        else if (result) cJSON_Delete(result);
        free(e->body);
        e->body = NULL;
        e->in_use = false;
        while (g_send_outbox_size > 0 && !g_send_outbox[g_send_outbox_head].in_use) {
            g_send_outbox_head = (g_send_outbox_head + 1) % SEND_OUTBOX_DEPTH;
            g_send_outbox_size--;
        }
        return;
    }

    if (result) cJSON_Delete(result);

    /* Permanent rejection: the server refused this message and retrying will
     * never help (403 removed from room, 401 dead token, 413 too large, 400
     * malformed, ...). Drop the entry so it can't become a head-of-line poison
     * that blocks every later send. 408 (timeout) and 429 (rate-limit) are the
     * transient exceptions and fall through to the retry path below. */
    long http = g_last_http_code;
    if (http >= 400 && http < 500 && http != 408 && http != 429) {
        fprintf(stderr, "matrix_async: send rejected by server (HTTP %ld), dropping\n", http);
        if (e->cb) e->cb(NULL, MATRIX_ERR_REJECTED, e->userdata);
        free(e->body);
        e->body = NULL;
        e->in_use = false;
        while (g_send_outbox_size > 0 && !g_send_outbox[g_send_outbox_head].in_use) {
            g_send_outbox_head = (g_send_outbox_head + 1) % SEND_OUTBOX_DEPTH;
            g_send_outbox_size--;
        }
        return;
    }

    /* Transient failure: advance the backoff and retry forever. Once the table
     * is exhausted, clamp attempts at SEND_BACKOFF_STEPS so the index stays on
     * the last (60s) step — the entry keeps retrying at that cap indefinitely,
     * and the counter can't overflow. */
    if (e->attempts < SEND_BACKOFF_STEPS) e->attempts++;
    uint32_t delay = SEND_BACKOFF_MS[e->attempts - 1];
    e->next_retry_ms = monotonic_ms() + delay;
    fprintf(stderr, "matrix_async: send attempt %d failed, retrying in %ums\n",
            e->attempts, delay);
}

void matrix_async_outbox_resume(void)
{
    /* Fast-track every in-use entry: reset its backoff schedule (attempts back
     * to 0, next_retry_ms cleared) so the head retries immediately on the next
     * tick instead of waiting out its remaining backoff. */
    for (int n = 0; n < SEND_OUTBOX_DEPTH; n++) {
        send_outbox_entry_t *e = &g_send_outbox[n];
        if (!e->in_use) continue;
        e->attempts      = 0;
        e->next_retry_ms = 0;
    }
}

void matrix_async_sync(const char *since, matrix_async_cb_t cb, void *userdata)
{
    if (!g_initialized || !g_curl) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }

    char url[1024];
    if (since && since[0] != '\0') {
        char *encoded = curl_easy_escape(g_curl, since, 0);
        if (!encoded) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }
        snprintf(url, sizeof(url),
                 "%s/_matrix/client/v3/sync?since=%s&timeout=0"
                 "&filter={\"room\":{\"timeline\":{\"limit\":50},"
                 "\"state\":{\"types\":[]}},"
                 "\"presence\":{\"types\":[]}}",
                 g_homeserver, encoded);
        curl_free(encoded);
    } else {
        snprintf(url, sizeof(url),
                 "%s/_matrix/client/v3/sync?timeout=0"
                 "&filter={\"room\":{\"timeline\":{\"limit\":50},"
                 "\"state\":{\"types\":[]}},"
                 "\"presence\":{\"types\":[]}}",
                 g_homeserver);
    }
    enqueue_request(REQ_TYPE_SYNC, REQ_METHOD_GET, url, NULL, cb, userdata);
}

void matrix_async_get_messages(const char *room_id, int limit,
                               matrix_async_cb_t cb, void *userdata)
{
    if (!g_initialized || !g_curl) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }

    char *encoded = curl_easy_escape(g_curl, room_id, 0);
    if (!encoded) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }
    char url[1024];
    snprintf(url, sizeof(url),
             "%s/_matrix/client/v3/rooms/%s/messages?dir=b&limit=%d",
             g_homeserver, encoded, limit);
    curl_free(encoded);
    enqueue_request(REQ_TYPE_GET_MESSAGES, REQ_METHOD_GET, url, NULL, cb, userdata);
}

void matrix_async_send_message(const char *room_id, const char *text,
                               matrix_async_cb_t cb, void *userdata)
{
    if (!g_initialized || !g_curl) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }

    /* Any prior backed-off entries get a fresh schedule because the user is
     * sending again — interpreted as "still want delivery". This must run even
     * when the outbox is full: fast-tracking the head is what lets a full outbox
     * start draining as fast as the network allows, so capacity can recover. */
    matrix_async_outbox_resume();

    if (g_send_outbox_size >= SEND_OUTBOX_DEPTH) {
        fprintf(stderr, "matrix_async: send outbox full, dropping message\n");
        if (cb) cb(NULL, MATRIX_ERR_QUEUE, userdata);
        return;
    }

    char *encoded = curl_easy_escape(g_curl, room_id, 0);
    if (!encoded) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }

    char txn_id[64];
    snprintf(txn_id, sizeof(txn_id), "chatbox_%u_%u",
             g_txn_counter++, (unsigned int)time(NULL));

    char url[1024];
    snprintf(url, sizeof(url),
             "%s/_matrix/client/v3/rooms/%s/send/m.room.message/%s",
             g_homeserver, encoded, txn_id);
    curl_free(encoded);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "msgtype", "m.text");
    cJSON_AddStringToObject(body, "body", text);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }

    int idx = g_send_outbox_tail;
    send_outbox_entry_t *e = &g_send_outbox[idx];
    memset(e, 0, sizeof(*e));
    strncpy(e->url, url, sizeof(e->url) - 1);
    e->url[sizeof(e->url) - 1] = '\0';
    e->body          = body_str;  /* outbox owns body_str until drain */
    e->cb            = cb;
    e->userdata      = userdata;
    e->attempts      = 0;
    e->next_retry_ms = 0;
    e->in_use           = true;

    g_send_outbox_tail = (g_send_outbox_tail + 1) % SEND_OUTBOX_DEPTH;
    g_send_outbox_size++;
}

void matrix_async_whoami(matrix_async_cb_t cb, void *userdata)
{
    if (!g_initialized) { if (cb) cb(NULL, MATRIX_ERR_TRANSPORT, userdata); return; }

    char url[512];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/account/whoami", g_homeserver);
    enqueue_request(REQ_TYPE_WHOAMI, REQ_METHOD_GET, url, NULL, cb, userdata);
}

/* -------------------------------------------------------------------------
 * Media download (incoming photos)
 * ---------------------------------------------------------------------- */

static bool mxc_server_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
}

static bool mxc_media_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

bool matrix_mxc_parse(const char *mxc, char *server, size_t server_n,
                      char *media_id, size_t media_n)
{
    static const char pfx[] = "mxc://";
    if (!mxc || !server || !media_id || server_n == 0 || media_n == 0) return false;
    if (strncmp(mxc, pfx, sizeof(pfx) - 1) != 0) return false;
    const char *s = mxc + sizeof(pfx) - 1;
    const char *slash = strchr(s, '/');
    if (!slash || slash == s) return false;
    size_t sl = (size_t)(slash - s);
    const char *m = slash + 1;
    size_t ml = strlen(m);
    if (ml == 0 || sl >= server_n || ml >= media_n) return false;
    for (size_t i = 0; i < sl; i++) if (!mxc_server_char(s[i])) return false;
    for (size_t i = 0; i < ml; i++) if (!mxc_media_char(m[i])) return false;
    memcpy(server, s, sl);
    server[sl] = '\0';
    memcpy(media_id, m, ml + 1);
    return true;
}

void matrix_async_get_media(const char *mxc, bool legacy, size_t max_bytes,
                            matrix_media_cb_t cb, void *userdata)
{
    char server[256], media[256];
    if (!matrix_mxc_parse(mxc, server, sizeof(server), media, sizeof(media))) {
        fprintf(stderr, "matrix_async: bad media uri, skipping\n");
        if (cb) cb(NULL, 0, 0, MATRIX_ERR_REJECTED, userdata);
        return;
    }
    if (!g_initialized || !g_curl) { if (cb) cb(NULL, 0, 0, MATRIX_ERR_TRANSPORT, userdata); return; }
    if (!g_multi) matrix_async_init();

    char url[1024];
    snprintf(url, sizeof(url), legacy ? "%s/_matrix/media/v3/download/%s/%s"
                                      : "%s/_matrix/client/v1/media/download/%s/%s",
             g_homeserver, server, media);
    if (g_media.in_use) {           /* one download at a time: ask again later */
        if (cb) cb(NULL, 0, 0, MATRIX_ERR_QUEUE, userdata);
        return;
    }
    async_request_t *r = &g_media;
    memset(r, 0, sizeof(*r));
    r->in_use    = true;
    r->type      = REQ_TYPE_MEDIA;
    r->method    = REQ_METHOD_GET;
    snprintf(r->url, sizeof(r->url), "%s", url);
    r->media_cb  = cb;
    r->max_bytes = max_bytes;
    r->userdata  = userdata;
}
