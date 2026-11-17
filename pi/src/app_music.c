/**
 * app_music.c — Music app: the app contract (app.h) around the music UI
 * (ui_music.c; docs/player-spec.md §7, §8.2–§8.4).
 *
 * The app's group holds one focusable key sink (D3), so Tab/LV_KEY_NEXT/
 * LV_KEY_PREV have nowhere to go. Every key the sink gets goes through
 * shell_filter_key() with the context of the view on screen (§8.1: Music
 * columns, Now Playing or No music yet), so the D2 record stays global,
 * then to ui_music_handle_key().
 *
 * Handle keys on LV_EVENT_KEY only, never CLICKED: a press that opened
 * music from the switcher is consumed by the shell, and RELEASED/CLICKED
 * must not act either. Esc never reaches the sink (the shell's read hook
 * takes it, filters it and asks on_key): on Now Playing music consumes it
 * (Now Playing closes, §8.3); elsewhere on_key returns false, so the shell
 * opens the switcher and music keeps playing.
 *
 * Widgets here must not join chat's (default) group: build them inside
 * shell_suspend_default_group()/shell_restore_default_group() when they
 * are created outside create() (create() already runs with no default).
 *
 * Live tile (T9): ui_music.c draws it (it owns the current track, the art
 * code and the fallback covers) and reports every track/state change to
 * music_tile_changed(), which asks the shell to re-render the tile — a
 * no-op unless the switcher is open.
 */

#include "app.h"
#include "shell.h"
#include "key_filter.h"
#include "ui_music.h"
#include "ui_theme.h"

#include <stdio.h>

static lv_obj_t   *g_root  = NULL;
static lv_obj_t   *g_sink  = NULL;
static lv_group_t *g_group = NULL;

static library_t                *g_lib = NULL;
static const music_player_ops_t *g_ops = NULL;

#ifdef CHATBOX_TESTING
static int  g_key_events = 0;      /* LV_EVENT_KEY seen (before filtering) */
static int  g_accepted[256];       /* accepted keys, by code (< 256) */
static bool g_ctx_forced = false;  /* app_music_test_set_ctx() */
static key_ctx_t g_forced_ctx = KEY_CTX_NO_MUSIC;
#endif

static key_ctx_t current_ctx(void)
{
#ifdef CHATBOX_TESTING
    if (g_ctx_forced) return g_forced_ctx;
#endif
    return ui_music_key_ctx();
}

static void sink_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
#ifdef CHATBOX_TESTING
    g_key_events++;
#endif

    /* Normally Esc never gets here (the shell's read hook takes it). */
    if (key == LV_KEY_ESC) {
        shell_handle_esc();
        return;
    }

    if (!shell_filter_key(key, current_ctx())) return;
#ifdef CHATBOX_TESTING
    if (key < 256) g_accepted[key]++;
#endif
    ui_music_handle_key(key);
}

static void root_delete_cb(lv_event_t *e)
{
    (void)e;
    g_root = NULL;
    g_sink = NULL;
}

static void music_tile_changed(void)
{
    shell_refresh_tile("music");
}

static void music_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);
    ui_music_create(root);
    ui_music_set_tile_listener(music_tile_changed);
    shell_attach_peek(ui_music_peek_pill(), "chat");

    /* The single focusable object of music's group (D3). */
    g_group = lv_group_create();
    g_sink = lv_obj_create(root);
    lv_obj_remove_style_all(g_sink);
    lv_obj_set_size(g_sink, 1, 1);
    lv_obj_clear_flag(g_sink, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_sink, sink_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g_group, g_sink);

    ui_music_set_sources(g_lib, g_ops);
}

static lv_group_t *music_group(void)
{
    return g_group;
}

static void music_on_show(void)
{
    if (g_sink) lv_group_focus_obj(g_sink);
    ui_music_on_show();
}

static void music_on_hide(void)
{
    /* Playback carries on in the background; only the animations stop. */
    ui_music_on_hide();
}

static bool music_on_key(uint32_t key)
{
    /* Esc on Now Playing goes back to the album page (§8.3), on the album
     * page back to the grid; on the grid and on "No music yet" it isn't
     * ours, so the shell opens the switcher (§8.2, §8.4). */
    if (key == LV_KEY_ESC) return ui_music_handle_esc();
    return false;
}

const app_t app_music = {
    .id          = "music",
    .label       = "Music",
    .look        = UI_APP_MUSIC,
    .create      = music_create,
    .group       = music_group,
    .on_show     = music_on_show,
    .on_hide     = music_on_hide,
    .on_key      = music_on_key,
    .tile_render = ui_music_tile_render,
    .badge       = NULL,   /* music has nothing unread */
    .peek_render = ui_music_peek_render,
    .hints       = ui_music_hints,
};

void app_music_configure(library_t *lib, const music_player_ops_t *ops)
{
    g_lib = lib;
    g_ops = ops;
    if (g_root) ui_music_set_sources(lib, ops);
}

void app_music_poll_library(void)
{
    if (g_root) ui_music_poll_library();
}

#ifdef CHATBOX_TESTING
lv_obj_t *app_music_get_root(void)      { return g_root; }
lv_obj_t *app_music_get_sink(void)      { return g_sink; }
int  app_music_get_key_events(void)     { return g_key_events; }
int  app_music_get_accepted(uint32_t k) { return k < 256 ? g_accepted[k] : 0; }
void app_music_test_reset_counters(void)
{
    g_key_events = 0;
    for (int i = 0; i < 256; i++) g_accepted[i] = 0;
}
/* Let a test filter as another music view would (e.g. the columns, where
 * Enter is one-shot) to check the global record across a context change.
 * Stays in force until app_music_test_clear_ctx(). */
void app_music_test_set_ctx(key_ctx_t ctx)
{
    g_ctx_forced = true;
    g_forced_ctx = ctx;
}
void app_music_test_clear_ctx(void)     { g_ctx_forced = false; }
#endif
