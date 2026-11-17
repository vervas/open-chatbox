/**
 * app.h — App contract for the shell (docs/player-spec.md §5).
 *
 * Every app owns one lv_obj root (a full-screen child of the active
 * screen) and one lv_group (D3 focus isolation). The shell creates each
 * app once, hidden, and switching only hides one root and shows another:
 * nothing is destroyed, so an app's scroll positions, typed text and
 * focus survive a trip through the switcher.
 *
 * Key delivery (see shell.c for the full picture):
 *   - Esc is intercepted by the shell before LVGL dispatches it. The shell
 *     asks the visible app's on_key(LV_KEY_ESC) first; if that returns
 *     false the shell opens the switcher.
 *   - Every other key reaches the app through LVGL: the keyboard indev
 *     points at the visible app's group, so the focused object in that
 *     group gets LV_EVENT_KEY as usual.
 */

#ifndef CHATBOX_APP_H
#define CHATBOX_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "config.h"
#include "ui_music.h"
#include "ui_hints.h"
#include "ui_theme.h"

typedef struct app {
    const char   *id;                       /* "chat", "music" */
    const char   *label;                    /* "Chat": the switcher tile's caption */
    ui_app_t      look;                     /* its colours (ui_theme.h) */
    void        (*create)(lv_obj_t *root);  /* build the UI once, hidden */
    lv_group_t *(*group)(void);             /* this app's own focus group (D3) */
    void        (*on_show)(void);
    void        (*on_hide)(void);
    bool        (*on_key)(uint32_t key);    /* true = handled, stop */
    void        (*tile_render)(lv_obj_t *tile); /* live tile content (T9) */
    int         (*badge)(void);             /* unread count, 0 = none */
    /* The other app's header shows this app's news in a small pill
     * (ui_peek_create; the shell calls this on every shell_refresh_tile
     * of this app). Fill the pill and return true, or return false to
     * hide it (nothing to say). */
    bool        (*peek_render)(lv_obj_t *pill);
    /* Key hints for what this app shows now (CHATBOX_KEY_HINTS): a static
     * list, its length in *n. */
    const ui_hint_t *(*hints)(int *n);
} app_t;

/* The two v1 apps (D11), and Photos (docs/photos.md). */
extern const app_t app_chat;
extern const app_t app_music;
extern const app_t app_photos;

/* The app id for config's default_app (§10): what shell_start() boots.
 * home boots chat behind the switcher (app_boot_home). */
static inline const char *app_id_for_config(config_app_t app)
{
    return app == CONFIG_APP_MUSIC ? "music" : "chat";
}

/* default_app = home: open the switcher once shell_start() has run. */
static inline bool app_boot_home(config_app_t app)
{
    return app == CONFIG_APP_HOME;
}

/* ---- Chat (app_chat.c) ---------------------------------------------------- */

/* Call before shell_start(): the contacts and the sidebar's select callback. */
void app_chat_configure(const config_t *cfg, void (*on_select)(int contact_idx));
/* Valid after shell_start(). main.c builds chat panels in the content panel. */
lv_obj_t *app_chat_sidebar_panel(void);
lv_obj_t *app_chat_content_panel(void);
/* After shell_start(): open the first contact's chat (the kiosk boots into
 * a conversation). No contacts: nothing; chat shows "No contacts yet". */
void app_chat_open_first(void);

/* ---- Music (app_music.c, ui_music.c) ------------------------------------- */

/* The library (NULL → "No music yet") and the player operations (NULL →
 * no playback; the firmware passes &music_player_real_ops). Call before or
 * after shell_start(); the music UI reloads. */
void app_music_configure(library_t *lib, const music_player_ops_t *ops);
/* library_poll() + reload on change (§7.7). From an lv_timer every 2 s. */
void app_music_poll_library(void);

/* ---- Photos (app_photos.c, ui_photos.c) ---------------------------------- */

#include "photo_lib.h"
/* The library (NULL → "No photos yet"). Before or after shell_start(). */
void app_photos_configure(photo_lib_t *lib);
/* photo_lib_poll() (force: a full rescan, e.g. chat just saved a photo)
 * and redraw on a change. From an lv_timer every 2 s. */
void app_photos_poll_library(bool force);
/* Chat saved a photo into the folder (photo_inbox's callback): the next
 * poll rescans, once however many were saved meanwhile. */
void app_photos_photo_saved(void);
/* The photo viewer is what's on screen (the screen-sleep override). */
bool app_photos_viewer_showing(void);

#ifdef CHATBOX_TESTING
lv_obj_t *app_photos_get_sink(void);
int  app_photos_get_accepted(uint32_t key);
void app_photos_test_reset_counters(void);
#endif

#ifdef CHATBOX_TESTING
/* ---- Chat test hooks (app_chat.c) ----------------------------------------- */
typedef struct {
    bool        built;        /* the tile has been rendered at least once */
    bool        bubble;       /* speech-bubble face (nobody unread) */
    bool        unread;       /* avatar + count face */
    int         newest;       /* contact shown, -1 = none */
    const void *avatar_src;   /* the avatar image's source */
    bool        initial;      /* no avatar: the name's initial instead */
    char        count[16];
    char        name[64];
} app_chat_tile_state_t;
void app_chat_test_tile(app_chat_tile_state_t *out);
bool app_chat_test_no_contacts(void);   /* "No contacts yet" is showing */

#include "key_filter.h"
/* ---- Music test hooks (app_music.c) --------------------------------------- */
lv_obj_t *app_music_get_root(void);
lv_obj_t *app_music_get_sink(void);
int  app_music_get_key_events(void);
int  app_music_get_accepted(uint32_t key);
void app_music_test_reset_counters(void);
void app_music_test_set_ctx(key_ctx_t ctx);   /* force a filter context... */
void app_music_test_clear_ctx(void);          /* ...until this (then: the view's) */
#endif

#endif /* CHATBOX_APP_H */
