/**
 * main.c — Chatbox LVGL kiosk entry point
 *
 * Supports two backends selected at compile time:
 *   CHATBOX_USE_SDL  — SDL2 window (Docker/dev)
 *   CHATBOX_USE_FB   — Linux framebuffer (Raspberry Pi)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "lvgl.h"

#ifdef CHATBOX_USE_SDL
#  include "src/drivers/sdl/lv_sdl_window.h"
#  include "src/drivers/sdl/lv_sdl_keyboard.h"
#  include "src/drivers/sdl/lv_sdl_mouse.h"
#  include LV_SDL_INCLUDE_PATH
#endif

#ifdef CHATBOX_USE_FB
#  include "src/drivers/display/fb/lv_linux_fbdev.h"
#  include "src/drivers/libinput/lv_libinput.h"
#  include "src/drivers/libinput/lv_libinput_private.h"   /* per-contact XKB layout */
#  include <pthread.h>
#  include <linux/kd.h>
#  include <linux/input.h>
#  include <sys/ioctl.h>
#  include <fcntl.h>
#endif

#include "cJSON.h"
#include "config.h"
#include "lang.h"
#include "matrix_api.h"
#include "msg_cache.h"
#include "sync_merge.h"
#include "ui_sidebar.h"
#include "ui_chat.h"
#include "emoji_font.h"
#include "avatars.h"
#include "display_sleep.h"
#include "app.h"
#include "shell.h"
#include "ui_theme.h"
#include "library.h"
#include "player.h"
#include "photo_lib.h"
#include "photo_inbox.h"
#include "prof.h"

/* Display resolution */
#define DISP_WIDTH  1280
#define DISP_HEIGHT  800

/* Poll period in milliseconds */
#define POLL_PERIOD_MS 3000

/* Poll timer handle */
static lv_timer_t *g_poll_timer = NULL;

/* Sync token for incremental Matrix sync */
static char g_sync_token[512] = {0};

/* Config and current chat */
static config_t  g_cfg;
static char      g_current_room[256];
static char      g_current_name[64];

/* Music library reader (docs/player-spec.md §7.7); NULL only on OOM. */
static library_t *g_music_lib = NULL;

/* Library generation poll period (§7.7) */
#define MUSIC_LIBRARY_POLL_MS 2000

/* The photo folder (docs/photos.md); NULL only on OOM. */
static photo_lib_t *g_photo_lib = NULL;
#define PHOTO_POLL_MS        2000
#define PHOTO_INBOX_TICK_MS   250

/* User ID of the kiosk account (extracted from first sync) */
static char g_my_user_id[256] = {0};

/* Split layout: sidebar (left) + content panel (right) */
static lv_obj_t *g_sidebar_panel  = NULL;
static lv_obj_t *g_content_panel  = NULL;
static lv_obj_t *g_active_panel   = NULL;  /* currently visible chat panel */
static int       g_current_contact = -1;

/* LRU panel cache (Step 5). Each entry holds a fully-built room panel
 * that lives as a child of g_content_panel. On switch-away the panel
 * is hidden (LV_OBJ_FLAG_HIDDEN); on switch-back we just unhide and
 * rebind ui_chat globals — no re-render. */
#define PANEL_CACHE_SIZE 3
typedef struct {
    char       room_id[256];
    lv_obj_t  *panel;
    uint64_t   last_used;
    bool       history_loaded;
} panel_cache_entry_t;

static panel_cache_entry_t g_panel_cache[PANEL_CACHE_SIZE] = {0};
static uint64_t            g_panel_lru_counter = 0;

static panel_cache_entry_t *panel_cache_find(const char *room_id)
{
    if (!room_id || !room_id[0]) return NULL;
    for (int i = 0; i < PANEL_CACHE_SIZE; i++) {
        if (g_panel_cache[i].panel &&
            strcmp(g_panel_cache[i].room_id, room_id) == 0) {
            return &g_panel_cache[i];
        }
    }
    return NULL;
}

static panel_cache_entry_t *panel_cache_alloc_slot(void)
{
    panel_cache_entry_t *lru = &g_panel_cache[0];
    for (int i = 0; i < PANEL_CACHE_SIZE; i++) {
        if (!g_panel_cache[i].panel) return &g_panel_cache[i];
        if (g_panel_cache[i].last_used < lru->last_used) lru = &g_panel_cache[i];
    }
    /* Evict LRU */
    if (lru->panel) {
        ui_chat_free_panel_state(lru->panel);
        lv_obj_delete(lru->panel);
        lru->panel = NULL;
    }
    lru->room_id[0]     = '\0';
    lru->last_used      = 0;
    lru->history_loaded = false;
    return lru;
}

/* Invariant: every panel in g_panel_cache must be destroyed via
 * panel_cache_alloc_slot (LRU eviction) or panel_cache_clear_all.
 * Direct lv_obj_delete() on a cached panel leaves a dangling pointer;
 * if a future call site needs to destroy one outside of this file,
 * add a panel_cache_invalidate(panel) helper here first. */
static void panel_cache_clear_all(void)
{
    for (int i = 0; i < PANEL_CACHE_SIZE; i++) {
        if (g_panel_cache[i].panel) {
            ui_chat_free_panel_state(g_panel_cache[i].panel);
            lv_obj_delete(g_panel_cache[i].panel);
        }
        g_panel_cache[i].panel          = NULL;
        g_panel_cache[i].room_id[0]     = '\0';
        g_panel_cache[i].last_used      = 0;
        g_panel_cache[i].history_loaded = false;
    }
    g_panel_lru_counter = 0;
}

static volatile int g_running = 1;

#ifdef CHATBOX_USE_FB
/* Current keyboard input device (for hotplug detection) */
static lv_indev_t *g_kb_indev = NULL;
static char g_kb_path[64] = {0};

/* Scan /dev/input/eventN for a keyboard device. Returns strdup'd path or NULL.
 * Uses EVIOCGBIT capability detection rather than name matching so dongles
 * that advertise themselves as "2.4G Wireless Receiver" etc. are picked up. */
static char *find_usb_keyboard(void)
{
    #define NBITS(x) (((x) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
    #define TEST_BIT(bit, arr) ((arr[(bit) / (8 * sizeof(long))] >> ((bit) % (8 * sizeof(long)))) & 1UL)
    for (int ev = 0; ev < 32; ev++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", ev);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long evbits[NBITS(EV_MAX)] = {0};
        unsigned long keybits[NBITS(KEY_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
            close(fd);
            continue;
        }
        close(fd);

        /* Must emit key events and have standard alphabetic keys.
         * Rules out mice, gamepads, power buttons, and HID consumer-control
         * interfaces that only carry media keys. */
        if (!TEST_BIT(EV_KEY, evbits)) continue;
        if (!TEST_BIT(KEY_A, keybits)) continue;
        if (!TEST_BIT(KEY_SPACE, keybits)) continue;
        return strdup(path);
    }
    return NULL;
    #undef TEST_BIT
    #undef NBITS
}

/* XKB layout for the open chat (lang_xkb_layout of its contact); NULL =
 * the default LV_LIBINPUT_XKB_KEY_MAP. */
static const char *g_kb_layout = NULL;

/* Load g_kb_layout into the keyboard's XKB keymap. LVGL's libinput worker
 * thread translates keys with it (lv_xkb_process_key) under the driver's
 * event_lock, so the swap takes that lock — the one place we touch another
 * thread's state. A layout that fails to load (missing xkb data) falls
 * back to the default rather than leaving the keyboard dead. */
static void kb_apply_layout(void)
{
#if LV_LIBINPUT_XKB
    if (!g_kb_indev) return;
    lv_libinput_t *dsc = lv_indev_get_driver_data(g_kb_indev);
    if (!dsc) return;

    struct xkb_rule_names def = LV_LIBINPUT_XKB_KEY_MAP;
    struct xkb_rule_names names = def;
    if (g_kb_layout) names.layout = g_kb_layout;

    pthread_mutex_lock(&dsc->event_lock);
    bool ok = lv_xkb_init(&dsc->xkb, names);
    if (!ok && g_kb_layout) lv_xkb_init(&dsc->xkb, def);
    pthread_mutex_unlock(&dsc->event_lock);

    if (ok)
        fprintf(stderr, "keyboard: layout %s\n", names.layout);
    else
        fprintf(stderr, "keyboard: layout %s failed to load, using %s\n",
                names.layout, def.layout);
#endif
}

static void kb_set_layout(const char *layout)
{
    if (layout == g_kb_layout ||
        (layout && g_kb_layout && strcmp(layout, g_kb_layout) == 0))
        return;
    g_kb_layout = layout;
    kb_apply_layout();
}

/* Attach a keyboard indev: the shell hooks its reads (key routing) and
 * points it at the *active* group — chat's, music's or the switcher's —
 * which matters on a hotplug re-attach while music or the switcher is up. */
static void attach_keyboard(const char *path)
{
    if (g_kb_indev) {
        lv_indev_delete(g_kb_indev);
        g_kb_indev = NULL;
    }
    g_kb_indev = lv_libinput_create(LV_INDEV_TYPE_KEYPAD, path);
    strncpy(g_kb_path, path, sizeof(g_kb_path) - 1);

    if (g_kb_indev) {
        shell_attach_keyboard(g_kb_indev);
        if (g_kb_layout) kb_apply_layout();   /* new indev starts on the default */
    }

    fprintf(stderr, "keyboard attached: %s\n", path);
}

/* Timer callback: check if the keyboard device is still valid, rescan if not */
static void kb_hotplug_cb(lv_timer_t *timer)
{
    (void)timer;
    PROF_START(t0);

    /* Check if current device is still accessible */
    if (g_kb_path[0] != '\0') {
        int fd = open(g_kb_path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            close(fd);
            PROF_LOG(t0, "kb_hotplug_cb (alive)");
            return;  /* still good */
        }
        /* Device gone — detach */
        fprintf(stderr, "keyboard disconnected: %s\n", g_kb_path);
        if (g_kb_indev) {
            lv_indev_delete(g_kb_indev);
            g_kb_indev = NULL;
        }
        g_kb_path[0] = '\0';
    }

    /* Try to find a new keyboard */
    char *new_kb = find_usb_keyboard();
    if (new_kb) {
        attach_keyboard(new_kb);
        free(new_kb);
    }
    PROF_LOG(t0, "kb_hotplug_cb (rescan)");
}
#else
/* SDL: typing goes through the OS keyboard layout */
static void kb_set_layout(const char *layout) { (void)layout; }
#endif

/* -------------------------------------------------------------------------
 * Message fetching & display
 * ---------------------------------------------------------------------- */

static bool event_is_mine(const cJSON *event)
{
    const cJSON *sender = cJSON_GetObjectItemCaseSensitive(event, "sender");
    return cJSON_IsString(sender) && g_my_user_id[0] != '\0' &&
           strcmp(sender->valuestring, g_my_user_id) == 0;
}

/* Display a single Matrix m.room.message event */
static void display_event(cJSON *event)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive(event, "content");
    if (!content) return;

    photo_event_t pe;
    if (photo_inbox_parse(event, &pe)) {
        ui_chat_add_photo(pe.event_id, pe.ts, pe.w, pe.h, event_is_mine(event));
        return;
    }

    cJSON *msgtype = cJSON_GetObjectItemCaseSensitive(content, "msgtype");
    if (!cJSON_IsString(msgtype) || strcmp(msgtype->valuestring, "m.text") != 0)
        return;

    cJSON *body = cJSON_GetObjectItemCaseSensitive(content, "body");
    if (!cJSON_IsString(body) || body->valuestring[0] == '\0')
        return;

    cJSON *sender = cJSON_GetObjectItemCaseSensitive(event, "sender");
    bool is_from_me = false;
    if (cJSON_IsString(sender) && g_my_user_id[0] != '\0')
        is_from_me = (strcmp(sender->valuestring, g_my_user_id) == 0);

    ui_chat_add_message(body->valuestring, is_from_me);
}

/* -------------------------------------------------------------------------
 * Batched history render — drains a room's cached events 5 per LVGL tick
 * so the main thread never blocks on a 50-message render. The drain owns a
 * copy of the room's msg_cache array (sync keeps appending to the cache,
 * and trims it); a sync event for the room that arrives mid-drain is added
 * to the copy, so it renders in order. A generation counter detects
 * cancellation when the user switches chats.
 * ---------------------------------------------------------------------- */

static cJSON       *g_pending_history    = NULL;  /* owned copy */
static char         g_pending_room[256]  = {0};
static int          g_pending_idx        = 0;
static int          g_pending_total      = 0;
static lv_timer_t  *g_history_drain_timer = NULL;
static uint32_t     g_show_chat_generation = 0;
static uint32_t     g_history_generation   = 0;

static void cancel_pending_history(void)
{
    if (g_history_drain_timer) {
        lv_timer_delete(g_history_drain_timer);
        g_history_drain_timer = NULL;
    }
    if (g_pending_history) cJSON_Delete(g_pending_history);
    g_pending_history = NULL;
    g_pending_room[0] = '\0';
    g_pending_idx     = 0;
    g_pending_total   = 0;
}

/* A sync event for the room being drained: render it after the rest. */
static bool drain_append(const char *room_id, const cJSON *event)
{
    if (!g_pending_history || strcmp(room_id, g_pending_room) != 0) return false;
    cJSON_AddItemToArray(g_pending_history, cJSON_Duplicate(event, 1));
    g_pending_total++;
    return true;
}

static void history_drain_cb(lv_timer_t *timer)
{
    PROF_START(t0);

    /* Stale fill from a previous chat — drop it. */
    if (g_history_generation != g_show_chat_generation) {
        cancel_pending_history();
        return;
    }

    if (!g_pending_history) {
        if (timer) lv_timer_delete(timer);
        g_history_drain_timer = NULL;
        return;
    }

    const int batch = 5;
    int processed = 0;
    while (processed < batch && g_pending_idx < g_pending_total) {
        cJSON *ev = cJSON_GetArrayItem(g_pending_history, g_pending_idx);
        if (ev) display_event(ev);
        g_pending_idx++;
        processed++;
    }

    if (g_pending_idx >= g_pending_total) {
        cancel_pending_history();
        PROF_LOG(t0, "history_drain_cb (final batch=%d)", processed);
        return;
    }

    PROF_LOG(t0, "history_drain_cb (%d msgs, %d/%d)",
             processed, g_pending_idx, g_pending_total);
}

static void start_history_drain(const char *room_id, const cJSON *cached)
{
    cancel_pending_history();
    if (!cached) return;
    cJSON *array = cJSON_Duplicate(cached, 1);
    if (!array) return;

    g_pending_history    = array;
    strncpy(g_pending_room, room_id, sizeof(g_pending_room) - 1);
    g_pending_room[sizeof(g_pending_room) - 1] = '\0';
    g_pending_idx        = 0;
    g_pending_total      = cJSON_GetArraySize(array);
    g_history_generation = g_show_chat_generation;

    /* First fire is "next tick"; subsequent fires every 16 ms (~one frame). */
    g_history_drain_timer = lv_timer_create(history_drain_cb, 16, NULL);
    if (g_history_drain_timer) {
        /* Trigger immediate first batch so the first bubbles appear ASAP. */
        lv_timer_ready(g_history_drain_timer);
    }
}

/* Reference to the "Loading…" placeholder added to the active panel
 * until its room's timeline is in msg_cache (the initial sync). Removed
 * when it renders (or the panel is hidden). Only the active panel shows
 * one, so a single global pointer suffices. */
static lv_obj_t *g_loading_label = NULL;

static void show_loading_placeholder(void)
{
    if (g_loading_label) return;
    if (!g_active_panel) return;
    /* Fish out msg_cont via the panel's state. We don't have a getter,
     * so just create the label inside g_active_panel — it'll be
     * positioned/styled to look like a hint. The simpler path: use
     * ui_chat_add_message with a special string. But that adds a real
     * bubble we'd then have to delete. Inline label is cleaner. */
    g_loading_label = lv_label_create(g_active_panel);
    lv_label_set_text(g_loading_label, "Loading…");
    lv_obj_set_style_text_font(g_loading_label, &font_chat_28, 0);
    lv_obj_set_style_text_color(g_loading_label, UI_C_INK_2, 0);
    lv_obj_align(g_loading_label, LV_ALIGN_CENTER, 0, 0);
}

static void clear_loading_placeholder(void)
{
    if (!g_loading_label) return;
    lv_obj_delete(g_loading_label);
    g_loading_label = NULL;
}

static void mark_history_loaded(const char *room_id);

/* Show a room's history: its timeline from msg_cache (seeded by the
 * initial sync, kept current by every sync), drained in batches. Not
 * there yet (initial sync pending, e.g. offline at boot): "Loading…", and
 * on_sync_done calls this again once it is. */
static void load_history(const char *room_id)
{
    cJSON *cached = msg_cache_get(room_id);
    if (!cached) {
        show_loading_placeholder();
        return;
    }
    clear_loading_placeholder();
    mark_history_loaded(room_id);
    start_history_drain(room_id, cached);
    fprintf(stderr, "chat: history shown for %s, %d events\n",
            room_id, cJSON_GetArraySize(cached));
}

/* Mark a panel cache entry's history_loaded flag as true (load_history).
 * Walks the panel cache to find the entry. */
static void mark_history_loaded(const char *room_id)
{
    panel_cache_entry_t *entry = panel_cache_find(room_id);
    if (entry) entry->history_loaded = true;
}

/* Find the contact index whose room_id matches, or -1 */
static int find_contact_by_room(const char *room_id)
{
    for (int i = 0; i < g_cfg.contact_count; i++) {
        if (strcmp(g_cfg.contacts[i].room_id, room_id) == 0)
            return i;
    }
    return -1;
}

/* True between matrix_async_sync enqueue and the callback firing.
 * Used to drop overlapping polls during a slow network. */
static bool g_sync_in_flight = false;

/* ---- Sync → rooms (sync_merge.h does the timeline, we do the screen) --- */

static bool sync_wants_room(const char *room_id, void *ud)
{
    (void)ud;
    return find_contact_by_room(room_id) >= 0;
}

/* The text an event shows as a bubble, or NULL (not an m.text with a body). */
static const char *event_text(const cJSON *event)
{
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(event, "content");
    const cJSON *msgtype = cJSON_GetObjectItemCaseSensitive(content, "msgtype");
    const cJSON *body    = cJSON_GetObjectItemCaseSensitive(content, "body");
    if (!cJSON_IsString(msgtype) || strcmp(msgtype->valuestring, "m.text") != 0) return NULL;
    if (!cJSON_IsString(body) || body->valuestring[0] == '\0') return NULL;
    return body->valuestring;
}

static void sync_on_event(const char *room_id, const cJSON *event, bool initial,
                          bool mine, bool local_echo, void *ud)
{
    (void)ud;
    /* A new event on a panel whose history is already up (or being
     * drained): add its bubble. The initial sync only seeds; panels show
     * it through load_history. Our own device's echo is already on screen
     * as the optimistic bubble. */
    /* A photo is saved into the photo folder whenever chat sees it, the
     * initial sync included (photos sent while the kiosk was off). */
    photo_event_t photo;
    bool is_photo = photo_inbox_parse(event, &photo);
    if (is_photo) photo_inbox_offer(event);
    if (!initial && !local_echo) {
        const char *text = event_text(event);
        panel_cache_entry_t *pe = panel_cache_find(room_id);
        if ((text || is_photo) && pe && pe->history_loaded && !drain_append(room_id, event)) {
            if (is_photo)
                ui_chat_add_photo_to(pe->panel, photo.event_id, photo.ts, photo.w, photo.h, mine);
            else
                ui_chat_add_message_to(pe->panel, text, mine);
        }
    }
    if (mine) return;

    display_sleep_notify_message();
    bool is_current = g_current_room[0] != '\0' && strcmp(room_id, g_current_room) == 0;
    if (is_current) {
        /* The initial sync is the past (the history about to render), not
         * news: no "New" divider for it. */
        if (!initial && lv_display_get_inactive_time(NULL) > POLL_PERIOD_MS)
            ui_chat_insert_new_divider();
    } else {
        int idx = find_contact_by_room(room_id);
        if (idx >= 0) ui_sidebar_set_unread(idx, true);
    }
}

/* Process a sync response. Takes ownership of `sync`. */
static void process_sync_payload(cJSON *sync)
{
    if (!sync) return;
    bool initial = g_sync_token[0] == '\0';

    /* Save next_batch token for incremental sync */
    cJSON *nb = cJSON_GetObjectItemCaseSensitive(sync, "next_batch");
    if (cJSON_IsString(nb)) {
        strncpy(g_sync_token, nb->valuestring, sizeof(g_sync_token) - 1);
        g_sync_token[sizeof(g_sync_token) - 1] = '\0';
    }

    const sync_merge_ops_t ops = {
        .wants_room = sync_wants_room,
        .on_event   = sync_on_event,
    };
    sync_merge_apply(sync, initial, g_my_user_id, &ops);
    cJSON_Delete(sync);

    if (initial) {
        /* A contact's room with no timeline in the sync (not joined, or
         * empty): an empty one, so its chat shows rather than "Loading…". */
        for (int i = 0; i < g_cfg.contact_count; i++) {
            if (!msg_cache_get(g_cfg.contacts[i].room_id))
                msg_cache_set(g_cfg.contacts[i].room_id, cJSON_CreateArray());
        }
    }

    /* The open chat waiting for its timeline: show it now. */
    panel_cache_entry_t *pe = g_current_room[0] ? panel_cache_find(g_current_room) : NULL;
    if (pe && !pe->history_loaded) load_history(g_current_room);
}

static void on_sync_done(cJSON *sync, int status, void *userdata)
{
    (void)userdata;
    g_sync_in_flight = false;
    PROF_START(t0);
    if (status == MATRIX_OK && sync) {
        process_sync_payload(sync);
        PROF_LOG(t0, "on_sync_done (parse)");
    } else if (sync) {
        cJSON_Delete(sync);
    }
}

/* Async sync — enqueue and return immediately. The actual work happens
 * in on_sync_done once the response arrives (or fails). */
static void poll_sync(void)
{
    if (g_sync_in_flight) return;  /* drop overlapping polls */
    g_sync_in_flight = true;
    matrix_async_sync(g_sync_token[0] ? g_sync_token : NULL,
                      on_sync_done, NULL);
}

/* -------------------------------------------------------------------------
 * Async whoami callback — sets g_my_user_id and clears the in-flight flag.
 * ---------------------------------------------------------------------- */

static bool g_whoami_in_flight = false;

static void on_whoami_done(cJSON *result, int status, void *userdata)
{
    (void)userdata;
    g_whoami_in_flight = false;

    if (status == MATRIX_OK && result) {
        cJSON *uid = cJSON_GetObjectItemCaseSensitive(result, "user_id");
        if (cJSON_IsString(uid)) {
            strncpy(g_my_user_id, uid->valuestring, sizeof(g_my_user_id) - 1);
            g_my_user_id[sizeof(g_my_user_id) - 1] = '\0';
            fprintf(stderr, "logged in as: %s\n", g_my_user_id);
        }
    }
    if (result) cJSON_Delete(result);
}

/* Ask who we are, unless known or already asked. The session starts here:
 * main() asks at boot, before anything else is queued, and the poll timer
 * retries until it answers. */
static void request_whoami(void)
{
    if (g_my_user_id[0] != '\0' || g_whoami_in_flight) return;
    g_whoami_in_flight = true;
    matrix_async_whoami(on_whoami_done, NULL);
}

/* -------------------------------------------------------------------------
 * Poll timer callback (runs inside LVGL — no threads)
 * ---------------------------------------------------------------------- */

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    PROF_START(t0);

    /* Self-heal: retry whoami if it failed at startup. No sync before it
     * answers: the timeline needs to know which events are ours. */
    request_whoami();
    if (g_my_user_id[0] == '\0') return;

    /* Self-heal: retry initial sync if it failed at startup. Now async —
     * the callback (on_sync_done) will set g_sync_token from next_batch
     * and process any rooms.join data. */
    if (g_sync_token[0] == '\0') {
        if (!g_sync_in_flight) {
            g_sync_in_flight = true;
            matrix_async_sync(NULL, on_sync_done, NULL);
        }
        return;  /* don't double-poll until we have a token */
    }

    /* Clear "New" divider once the user is active again */
    if (lv_display_get_inactive_time(NULL) < POLL_PERIOD_MS)
        ui_chat_clear_new_divider();

    poll_sync();
    PROF_LOG(t0, "poll_timer_cb (sync)");
}

/* -------------------------------------------------------------------------
 * Send callback (called by ui_chat when user presses Send)
 * ---------------------------------------------------------------------- */

/* Send-completion callback. The bubble was already rendered optimistically
 * by ui_chat.do_send before we get here. `userdata` is the ui_chat
 * send-context handed to us via on_send; ui_chat_pending_complete uses it
 * to find the original bubble and (on failure) recolour it red. */
static void on_send_done(cJSON *result, int status, void *userdata)
{
    bool failed = (status != MATRIX_OK);
    if (failed) {
        fprintf(stderr, "matrix_async_send_message failed: status=%d\n", status);
    } else {
        /* Its echo then counts as ours even without a transaction_id. */
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "event_id");
        if (cJSON_IsString(id)) sync_merge_note_sent(id->valuestring);
    }
    ui_chat_pending_complete(userdata, failed);
    if (result) cJSON_Delete(result);
}

static void on_send(const char *text, void *send_ctx)
{
    if (g_current_room[0] == '\0') {
        /* No room selected — release the pending slot so it doesn't leak. */
        ui_chat_pending_complete(send_ctx, true);
        return;
    }
    matrix_async_send_message(g_current_room, text, on_send_done, send_ctx);
}

/* -------------------------------------------------------------------------
 * Screen navigation (split layout)
 * ---------------------------------------------------------------------- */

static void show_chat(int contact_idx);

static void on_contact_selected(int contact_idx)
{
    show_chat(contact_idx);
}

/* Build the app shell: music and chat roots (chat is the sidebar + content
 * panel split layout, built by app_chat.c) and the switcher, then show
 * default_app with the first contact's chat open. Tile order = registration
 * order: Music, Chat, Photos. */
static void build_shell(void)
{
    app_chat_configure(&g_cfg, on_contact_selected);
    shell_register_app(&app_music, true);   /* filters its own keys */
    shell_register_app(&app_chat, false);   /* takes LVGL keys directly */
    shell_register_app(&app_photos, true);  /* filters its own keys */
    shell_start(app_id_for_config(g_cfg.default_app));

    g_sidebar_panel = app_chat_sidebar_panel();
    g_content_panel = app_chat_content_panel();
    app_chat_open_first();
    if (app_boot_home(g_cfg.default_app)) shell_open_switcher();
}

static void async_tick_cb(lv_timer_t *t)
{
    (void)t;
    matrix_async_tick();
}

/* mpv supervision + IPC (never blocks); player callbacks fire from here. */
static void player_tick_cb(lv_timer_t *t)
{
    (void)t;
    player_tick();
}

static void library_poll_cb(lv_timer_t *t)
{
    (void)t;
    app_music_poll_library();
}

static void photo_poll_cb(lv_timer_t *t)
{
    (void)t;
    app_photos_poll_library(false);
}

static void photo_inbox_tick_cb(lv_timer_t *t)
{
    (void)t;
    photo_inbox_tick(lv_tick_get());
}

/* photo_inbox finished with a photo: chat's bubbles of it update, and a
 * saved one shows up in Photos at once. */
static void on_photo_done(const char *event_id, photo_status_t st, void *ud)
{
    (void)ud;
    ui_chat_photo_changed(event_id);
    if (st == PHOTO_ST_READY) app_photos_photo_saved();
}

static lv_obj_t *create_room_panel(void)
{
    lv_obj_t *panel = lv_obj_create(g_content_panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    return panel;
}

static void show_chat(int contact_idx)
{
    if (contact_idx < 0 || contact_idx >= g_cfg.contact_count) return;
    if (contact_idx == g_current_contact) return;  /* already showing */

    PROF_START(t0);

    /* Bump generation counter so any in-flight history drain becomes stale */
    g_show_chat_generation++;
    cancel_pending_history();

    /* Snapshot the currently-active panel's state and detach. The panel
     * stays alive in the cache; we just hide it. */
    clear_loading_placeholder();
    if (g_active_panel) {
        ui_chat_detach();
        lv_obj_add_flag(g_active_panel, LV_OBJ_FLAG_HIDDEN);
        g_active_panel = NULL;
    }

    g_current_contact = contact_idx;
    ui_sidebar_set_unread(contact_idx, false);

    strncpy(g_current_room, g_cfg.contacts[contact_idx].room_id,
            sizeof(g_current_room) - 1);
    g_current_room[sizeof(g_current_room) - 1] = '\0';

    strncpy(g_current_name, g_cfg.contacts[contact_idx].name,
            sizeof(g_current_name) - 1);
    g_current_name[sizeof(g_current_name) - 1] = '\0';

    ui_sidebar_set_selected(contact_idx);

    panel_cache_entry_t *entry = panel_cache_find(g_current_room);
    bool was_cached = (entry != NULL);

    if (!entry) {
        /* Cache miss: build a fresh panel and remember it. */
        entry = panel_cache_alloc_slot();
        entry->panel = create_room_panel();
        strncpy(entry->room_id, g_current_room, sizeof(entry->room_id) - 1);
        entry->room_id[sizeof(entry->room_id) - 1] = '\0';
        entry->history_loaded = false;
        ui_chat_create(entry->panel, g_current_name, g_current_room);
        ui_chat_set_avatar(ui_sidebar_avatar_src(contact_idx));
    } else {
        /* Cache hit: just unhide and re-bind globals. */
        lv_obj_clear_flag(entry->panel, LV_OBJ_FLAG_HIDDEN);
        ui_chat_attach(entry->panel);
    }

    entry->last_used = ++g_panel_lru_counter;
    g_active_panel   = entry->panel;
    ui_chat_set_send_callback(on_send);

    /* Typing language: accent combo on the panel, layout on the keyboard
     * (lang.h) */
    chat_lang_t lang = g_cfg.contacts[contact_idx].lang;
    ui_chat_set_language(lang);
    kb_set_layout(lang_xkb_layout(lang));

    /* Show the history only on first open of this room. Cached panels
     * keep their bubbles in the widget tree, so re-display would duplicate.
     * load_history sets the flag once the room's timeline is in msg_cache;
     * until then the panel shows "Loading…" and the next sync retries. */
    if (!entry->history_loaded) {
        load_history(g_current_room);
    }

    if (was_cached) {
        PROF_LOG(t0, "show_chat (cache hit) idx=%d", contact_idx);
    } else {
        PROF_LOG(t0, "show_chat (cache miss) idx=%d", contact_idx);
    }
}

/* -------------------------------------------------------------------------
 * Signal handlers
 * ---------------------------------------------------------------------- */

static void sigterm_handler(int sig)
{
    (void)sig;
    /* Graceful shutdown: cleanup LVGL and Matrix client, then exit */
    display_sleep_deinit();
    player_deinit();        /* don't leave mpv playing on its own */
    lv_deinit();
    matrix_cleanup();
#ifdef CHATBOX_USE_FB
    /* Restore text mode so the console works after we exit */
    {
        int tty_fd = open("/dev/tty0", O_RDWR);
        if (tty_fd < 0) tty_fd = open("/dev/tty1", O_RDWR);
        if (tty_fd >= 0) {
            ioctl(tty_fd, KDSETMODE, KD_TEXT);
            close(tty_fd);
        }
    }
#endif
    exit(0);
}

/* -------------------------------------------------------------------------
 * Tick callback
 * ---------------------------------------------------------------------- */

static uint32_t tick_cb(void)
{
#ifdef CHATBOX_USE_SDL
    extern uint32_t SDL_GetTicks(void);
    return SDL_GetTicks();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

#ifdef CHATBOX_USE_SDL
/* -------------------------------------------------------------------------
 * SDL key-up source (docs/player-spec.md §8.1 release reset)
 *
 * LVGL's SDL keyboard driver turns every SDL_KEYDOWN, OS auto-repeats
 * included, into a press + an immediate fake release and ignores
 * SDL_KEYUP, so the shell can't see real releases through the indev. This
 * event watch reports them. SDL calls it on the LVGL thread when it queues
 * an event (SDL_PumpEvents, inside SDL_PollEvent in LVGL's SDL timer),
 * possibly before LVGL has handled earlier queued events — so it reports
 * the presses too, and the shell pairs them up (shell.c, ext_take).
 * ---------------------------------------------------------------------- */

/* The SDL keys whose LVGL codes the one-shot rule cares about, mapped as
 * lv_sdl_keyboard.c + pi/patches/lvgl-keyboard-drivers.patch map them.
 * Space isn't here: LVGL takes it from SDL_TEXTINPUT. 0 = not tracked. */
static uint32_t sdl_tracked_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RIGHT:
    case SDLK_KP_PLUS:  return LV_KEY_RIGHT;
    case SDLK_LEFT:
    case SDLK_KP_MINUS: return LV_KEY_LEFT;
    case SDLK_UP:       return LV_KEY_UP;
    case SDLK_DOWN:     return LV_KEY_DOWN;
    case SDLK_ESCAPE:   return LV_KEY_ESC;
    case SDLK_BACKSPACE: return LV_KEY_BACKSPACE;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return LV_KEY_ENTER;
    case SDLK_PAGEUP:   return 0x05;   /* patched: emoji page up */
    case SDLK_PAGEDOWN: return 0x06;   /* patched: emoji page down */
    case SDLK_DELETE:   return LV_KEY_DEL;   /* Photos: delete (one-shot) */
    default:            return 0;
    }
}

static int sdl_key_watch(void *userdata, SDL_Event *e)
{
    (void)userdata;
    switch (e->type) {
    case SDL_KEYDOWN: {
        uint32_t key = sdl_tracked_key(e->key.keysym.sym);
        if (key) shell_key_pushed(key);
        break;
    }
    case SDL_KEYUP: {
        uint32_t key = e->key.keysym.sym == SDLK_SPACE ? ' '
                     : sdl_tracked_key(e->key.keysym.sym);
        if (key) shell_key_released(key);
        break;
    }
    case SDL_TEXTINPUT:
        /* LVGL delivers each byte of the text as a key press. */
        for (const char *c = e->text.text; *c; c++) {
            if (*c == ' ') shell_key_pushed(' ');
        }
        break;
    default:
        break;
    }
    return 1;   /* ignored for a watch */
}
#endif

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Signal handling: SIGINT is ignored (kiosk mode), SIGTERM exits cleanly */
    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, sigterm_handler);

    /* Parse config file */
    const char *cfg_path = (argc > 1) ? argv[1] : "/etc/chatbox.conf";
    if (config_parse(cfg_path, &g_cfg) != 0) {
        fprintf(stderr, "Failed to parse config: %s\n", cfg_path);
        return 1;
    }

    /* Init Matrix API. The session starts with whoami, first in the queue:
     * the timeline needs to know which events are ours (no sync before it
     * answers, poll_timer_cb). */
    matrix_init(g_cfg.homeserver_url, g_cfg.access_token);
    request_whoami();

    /* Both are async: whoami is queued above and runs once the LVGL loop
     * drives the dispatcher; the poll timer sends the initial sync after
     * it answers. Boot never blocks on the network. */

    /* Init LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    /* Shell first: keyboards attached below are hooked by it. */
    shell_init();

    /* Init display backend */
    lv_display_t *disp = NULL;

#ifdef CHATBOX_USE_SDL
    disp = lv_sdl_window_create(DISP_WIDTH, DISP_HEIGHT);
    if (!disp) {
        fprintf(stderr, "Failed to create SDL window\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "Chatbox");
    lv_indev_t *kb_indev = lv_sdl_keyboard_create();
    shell_attach_keyboard(kb_indev);
    /* Its release edges are fake: real key-ups come from the watch. */
    shell_keyboard_use_external_release(kb_indev, true);
    SDL_AddEventWatch(sdl_key_watch, NULL);
    lv_sdl_mouse_create();
#endif

#ifdef CHATBOX_USE_FB
    /* Switch VT to graphics mode so kernel console text doesn't cover LVGL */
    {
        int tty_fd = open("/dev/tty0", O_RDWR);
        if (tty_fd < 0) tty_fd = open("/dev/tty1", O_RDWR);
        if (tty_fd >= 0) {
            ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
            close(tty_fd);
        }
    }

    disp = lv_linux_fbdev_create();
    if (!disp) {
        fprintf(stderr, "Failed to create framebuffer display\n");
        return 1;
    }
    lv_linux_fbdev_set_file(disp, "/dev/fb0");
    /* Find and attach USB keyboard (with hotplug support).
     * lv_libinput_find_dev returns a pointer into its internal static cache —
     * borrowed, not owned. Don't free it. find_usb_keyboard's strdup'd path
     * we do own. */
    {
        char *kb = find_usb_keyboard();
        if (kb) {
            attach_keyboard(kb);
            free(kb);
        } else {
            const char *borrowed = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_KEYBOARD, true);
            if (borrowed) attach_keyboard(borrowed);
        }
    }
    /* Check for keyboard hotplug every 2 seconds */
    lv_timer_create(kb_hotplug_cb, 2000, NULL);

    /* Attach touchscreen if present (built-in, no hotplug needed).
     * Returned pointer is borrowed — do not free. */
    {
        const char *touch = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_TOUCH, true);
        if (touch) {
            lv_libinput_create(LV_INDEV_TYPE_POINTER, touch);
            fprintf(stderr, "touchscreen attached: %s\n", touch);
        }
    }
#endif

    (void)disp;

    /* Init emoji image font (use local assets for SDL2 dev, Pi path for FB) */
#ifdef CHATBOX_USE_SDL
    emoji_font_init("pi/src/assets/emoji");
    avatar_init("avatars");
#else
    emoji_font_init("/usr/share/chatbox/emoji");
    avatar_init("/usr/share/chatbox/avatars");
#endif

    /* Music: the player (mpv is spawned by the first player_tick; socket
     * from $CHATBOX_MPV_SOCKET or the default) and the library reader (a
     * missing DB is fine: "No music yet" until it appears). */
    {
        player_config_t pc;
        memset(&pc, 0, sizeof(pc));
        pc.volume_max     = g_cfg.music_volume_max;
        pc.volume_default = g_cfg.music_volume_default;
        player_init(&pc);
        g_music_lib = library_open(g_cfg.music_db, g_cfg.music_art_dir);
        app_music_configure(g_music_lib, &music_player_real_ops);
    }

    /* Photos: the folder (created by the inbox if missing) and the inbox
     * that fills it from chat. */
    photo_inbox_init(g_cfg.photos_dir, on_photo_done, NULL);
    g_photo_lib = photo_lib_open(g_cfg.photos_dir);
    app_photos_configure(g_photo_lib);

    /* Build the shell: apps (chat's split layout, music) + switcher */
    build_shell();

    /* Display sleep / lock screen */
    display_sleep_init(g_cfg.sleep_lock_secs, g_cfg.sleep_off_secs);
    /* The photo viewer is for looking, not typing: it stays awake longer. */
    display_sleep_set_lock_override(app_photos_viewer_showing,
                                    g_cfg.sleep_photo_viewer_mins * 60);

    /* Async dispatcher tick — drives curl_multi every ~16 ms so HTTP
     * requests run without blocking the LVGL main thread. */
    lv_timer_create(async_tick_cb, 16, NULL);

    /* Music: player I/O every ~16 ms, library generation every 2 s. */
    lv_timer_create(player_tick_cb, 16, NULL);
    lv_timer_create(library_poll_cb, MUSIC_LIBRARY_POLL_MS, NULL);

    /* Photos: the folder every 2 s, chat downloads a few times a second. */
    lv_timer_create(photo_poll_cb, PHOTO_POLL_MS, NULL);
    lv_timer_create(photo_inbox_tick_cb, PHOTO_INBOX_TICK_MS, NULL);

    /* The one poll timer: whoami retries, the initial sync, then
     * incremental syncs every POLL_PERIOD_MS, whatever chat is open. */
    g_poll_timer = lv_timer_create(poll_timer_cb, POLL_PERIOD_MS, NULL);

    /* Main loop */
    while (g_running) {
        PROF_START(tick_t0);
        uint32_t delay_ms = lv_timer_handler();
        PROF_WARN_IF_OVER(tick_t0, 100, "main-loop tick");
        if (delay_ms > 10) delay_ms = 10;
        usleep(delay_ms * 1000);
    }

    /* Cleanup */
    display_sleep_deinit();
    if (g_poll_timer) {
        lv_timer_delete(g_poll_timer);
        g_poll_timer = NULL;
    }
    cancel_pending_history();
    panel_cache_clear_all();
    msg_cache_clear_all();
    matrix_cleanup();
    player_deinit();
    lv_deinit();
    app_music_configure(NULL, NULL);
    library_close(g_music_lib);
    g_music_lib = NULL;
    photo_inbox_deinit();
    photo_lib_close(g_photo_lib);
    g_photo_lib = NULL;

    return 0;
}
