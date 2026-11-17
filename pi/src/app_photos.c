/**
 * app_photos.c — Photos app: the app contract (app.h) around the photos UI
 * (ui_photos.c; docs/photos.md).
 *
 * Like music: the app's group holds one focusable key sink (D3), every key
 * the sink gets goes through shell_filter_key() with KEY_CTX_PHOTOS, then
 * to ui_photos_handle_key(). Keys act on LV_EVENT_KEY only. Esc never
 * reaches the sink (the shell's read hook asks on_key): the confirm
 * sticker and the viewer consume it; on the grid and "No photos yet" the
 * shell opens the switcher.
 *
 * The screen-sleep override (display_sleep_set_lock_override, set up in
 * main.c) asks app_photos_viewer_showing(): true only while the viewer is
 * what's on screen — Photos visible, the viewer open, the switcher closed.
 */

#include "app.h"
#include "shell.h"
#include "key_filter.h"
#include "ui_photos.h"
#include "ui_theme.h"

static lv_obj_t    *g_root  = NULL;
static lv_obj_t    *g_sink  = NULL;
static lv_group_t  *g_group = NULL;
static photo_lib_t *g_lib   = NULL;
static bool         g_shown = false;
static bool         g_saved = false;  /* chat saved a photo since the last rescan */

#ifdef CHATBOX_TESTING
static int g_accepted[256];
#endif

static void sink_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {        /* normally the shell's read hook takes it */
        shell_handle_esc();
        return;
    }
    if (!shell_filter_key(key, ui_photos_key_ctx())) return;
#ifdef CHATBOX_TESTING
    if (key < 256) g_accepted[key]++;
#endif
    ui_photos_handle_key(key);
}

static void root_delete_cb(lv_event_t *e)
{
    (void)e;
    g_root = NULL;
    g_sink = NULL;
    g_shown = false;
}

static void photos_tile_changed(void)
{
    shell_refresh_tile("photos");
}

static void photos_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);
    ui_photos_create(root);
    ui_photos_set_tile_listener(photos_tile_changed);

    g_group = lv_group_create();
    g_sink = lv_obj_create(root);
    lv_obj_remove_style_all(g_sink);
    lv_obj_set_size(g_sink, 1, 1);
    lv_obj_clear_flag(g_sink, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_sink, sink_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g_group, g_sink);

    ui_photos_set_library(g_lib);
}

static lv_group_t *photos_group(void) { return g_group; }

static void photos_on_show(void)
{
    g_shown = true;
    if (g_sink) lv_group_focus_obj(g_sink);
    ui_photos_on_show();
}

static void photos_on_hide(void)
{
    g_shown = false;
    ui_photos_on_hide();
}

static bool photos_on_key(uint32_t key)
{
    if (key == LV_KEY_ESC) return ui_photos_handle_esc();
    return false;
}

const app_t app_photos = {
    .id          = "photos",
    .label       = "Photos",
    .look        = UI_APP_PHOTOS,
    .create      = photos_create,
    .group       = photos_group,
    .on_show     = photos_on_show,
    .on_hide     = photos_on_hide,
    .on_key      = photos_on_key,
    .tile_render = ui_photos_tile_render,
    .badge       = NULL,
    .peek_render = NULL,        /* Photos doesn't peek into other apps */
    .hints       = ui_photos_hints,
};

void app_photos_configure(photo_lib_t *lib)
{
    g_lib = lib;
    if (g_root) ui_photos_set_library(lib);
}

void app_photos_poll_library(bool force)
{
    if (!g_lib) return;
    bool changed = (force || g_saved) ? photo_lib_rescan(g_lib) : photo_lib_poll(g_lib);
    g_saved = false;
    if (changed && g_root) ui_photos_reload();
}

/* Not a rescan per photo: a backlog saved in a burst (photos sent while
 * the kiosk was off) is listed once, by the next 2 s poll. */
void app_photos_photo_saved(void)
{
    g_saved = true;
}

bool app_photos_viewer_showing(void)
{
    return g_root && g_shown && !shell_switcher_is_open() &&
           ui_photos_view() == PHOTOS_VIEW_VIEWER;
}

#ifdef CHATBOX_TESTING
lv_obj_t *app_photos_get_sink(void)      { return g_sink; }
int  app_photos_get_accepted(uint32_t k) { return k < 256 ? g_accepted[k] : 0; }
void app_photos_test_reset_counters(void)
{
    for (int i = 0; i < 256; i++) g_accepted[i] = 0;
}
#endif
