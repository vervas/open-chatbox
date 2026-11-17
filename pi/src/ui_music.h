/**
 * ui_music.h — Music app UI (docs/player-spec.md §7, §8.2–§8.4; mockup A).
 *
 * Views (one at a time, inside music's app root):
 *   NO_MUSIC   "No music yet": the library is missing, empty or broken.
 *   BROWSE     the album grid (focus MUSIC_COL_ALBUMS) or one album's page
 *              with its songs (focus MUSIC_COL_SONGS), over the mini
 *              now-playing strip. The grid is every album in the library
 *              (artist grouping is deferred, §12).
 *   NOW_PLAYING  the full-screen Now Playing page.
 * The volume overlay can show on top of any of them.
 *
 * The UI reads the library through library.h and drives playback through a
 * music_player_ops_t table: the firmware passes music_player_real_ops (the
 * player.h functions), tests pass a fake. Player callbacks are expected to
 * fire from the player's own tick only (player.h), never from inside an
 * operation.
 *
 * Keys arrive already filtered (shell_filter_key with ui_music_key_ctx()),
 * from app_music.c's key sink. Esc comes through ui_music_handle_esc():
 * music consumes it on Now Playing (back to the album page) and on the
 * album page (back to the grid).
 *
 * Threading: LVGL thread only.
 */

#ifndef CHATBOX_UI_MUSIC_H
#define CHATBOX_UI_MUSIC_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "key_filter.h"
#include "library.h"
#include "player.h"

/* The player operations the UI uses (player.h semantics). */
typedef struct {
    void (*set_callbacks)(const player_callbacks_t *cb);
    bool (*play_queue)(const player_track_t *tracks, int n, int start);
    void (*toggle)(void);       /* Space: playing ↔ paused only        */
    void (*restart)(void);      /* Enter on Now Playing                */
    void (*prev)(void);
    bool (*next)(void);         /* false: last track, nothing done     */
    int  (*volume_step)(int delta);
    player_state_t (*state)(void);
    int64_t (*current_id)(void);
    const char *(*current_path)(void);
    int  (*current_index)(void);
    int  (*queue_len)(void);
    int64_t (*position_ms)(void);
    int64_t (*duration_ms)(void);
    int  (*volume)(void);
    int  (*volume_max)(void);
} music_player_ops_t;

/* player.h, as is. */
extern const music_player_ops_t music_player_real_ops;

typedef enum {
    MUSIC_VIEW_NO_MUSIC = 0,
    MUSIC_VIEW_BROWSE,
    MUSIC_VIEW_NOW_PLAYING,
} music_view_t;

enum { MUSIC_COL_ALBUMS = 0, MUSIC_COL_SONGS, MUSIC_COL_COUNT };

/* Edge bounce directions (feedback for a key that has nowhere to go). */
enum { MUSIC_BOUNCE_NONE = 0, MUSIC_BOUNCE_UP, MUSIC_BOUNCE_DOWN, MUSIC_BOUNCE_LEFT,
       MUSIC_BOUNCE_RIGHT };

/* Build every widget inside `root` (hidden views). */
void ui_music_create(lv_obj_t *root);

/* Library (may be NULL: "No music yet") and player (NULL: keys that need a
 * player do nothing). Either may be set before or after create(); a change
 * reloads what is shown. */
void ui_music_set_sources(library_t *lib, const music_player_ops_t *ops);

/* library_poll() and, when it reports a change, reload the lists (§7.7). */
void ui_music_poll_library(void);

/* Reload the lists from the library now, keeping selections by id (§7.4). */
void ui_music_reload(void);

/* The §8.1 context of the current view (for the key filter). */
key_ctx_t ui_music_key_ctx(void);

/* Act on a key that already passed the filter. */
void ui_music_handle_key(uint32_t key);

/* An accepted Esc (the shell asks music's on_key first, §5 step 4): on Now
 * Playing, close it (the playing album's page) and return true; on an
 * album page, back to the grid and return true; on the grid and on "No
 * music yet" return false (the shell opens the switcher). */
bool ui_music_handle_esc(void);

void ui_music_on_show(void);
void ui_music_on_hide(void);

music_view_t ui_music_view(void);

/* Live tile (T9): build (first call) or refresh the Music tile's content
 * inside `face` (the switcher's tile area): the current album art, title
 * and play/pause state, or a music note when nothing is loaded. */
void ui_music_tile_render(lv_obj_t *face);

/* Called whenever what the tile shows may have changed (track, play state,
 * library reload). One listener; NULL to stop. */
void ui_music_set_tile_listener(void (*cb)(void));

/* Music's peek pill in chat's header (app_t.peek_render): the playing
 * song. False when nothing is loaded. */
bool ui_music_peek_render(lv_obj_t *pill);

/* The pill in music's own header where chat peeks in (for app_music.c to
 * hand to shell_attach_peek). */
lv_obj_t *ui_music_peek_pill(void);

/* Key hints for the view on screen (CHATBOX_KEY_HINTS). */
#include "ui_hints.h"
const ui_hint_t *ui_music_hints(int *n);

#ifdef CHATBOX_TESTING
/* What a test can observe. Per list (ALBUMS = the grid's cards, SONGS = the
 * album page's rows); sel_row_* is the card or row bound to the selection. */
typedef struct {
    music_view_t view;
    int      focus_col;               /* ALBUMS: the grid; SONGS: the album page */
    bool     grid_visible;
    bool     page_visible;
    char     page_title[128];         /* the album page's title */
    bool     peek_visible;            /* chat's pill in our header */
    int      count[MUSIC_COL_COUNT];      /* items in each list        */
    int      sel[MUSIC_COL_COUNT];        /* selected index, -1 = none */
    int64_t  sel_id[MUSIC_COL_COUNT];
    int      row_widgets[MUSIC_COL_COUNT];/* row widgets that exist    */
    int      rows_bound[MUSIC_COL_COUNT]; /* ...and are showing an item */
    int      rows_visible[MUSIC_COL_COUNT];/* ...that aren't hidden (bound or not) */
    int64_t  marked_id[MUSIC_COL_COUNT];  /* item with the playing marker among
                                             the bound rows, 0 = none */
    int      marked_rows[MUSIC_COL_COUNT];/* rows showing the marker    */
    bool     sel_row_fallback[MUSIC_COL_COUNT]; /* selected row draws the fallback cover */
    bool     sel_row_image[MUSIC_COL_COUNT];    /* ...or a loaded image */
    char     sel_row_letter[MUSIC_COL_COUNT][8];
    bool     sel_row_focused[MUSIC_COL_COUNT];  /* ...wears the focus ring */
    int      bounces;
    int      last_bounce;                 /* MUSIC_BOUNCE_* */
    bool     eq_animating;
    /* strip */
    bool     strip_visible;
    bool     strip_idle;
    char     strip_title[128];
    char     strip_sub[256];
    char     strip_time[32];
    bool     strip_fallback;
    /* now playing */
    char     np_state[32];                /* "Now Playing" / "Paused" */
    char     np_title[128];
    char     np_artist[128];
    char     np_album[128];
    char     np_count[64];
    char     np_elapsed[16];
    char     np_remaining[16];
    int      np_progress;                 /* 0..1000 */
    bool     np_fallback;
    bool     np_image;
    bool     np_paused_badge;
    /* volume overlay */
    bool     vol_visible;
    char     vol_value[16];
    char     vol_msg[64];
    int      vol_segs_on;
    int      vol_segs_locked;
    /* switcher tile */
    bool     tile_built;
    bool     tile_idle;               /* the note face */
    char     tile_title[128];
    char     tile_glyph[8];           /* LV_SYMBOL_PLAY / LV_SYMBOL_PAUSE */
    bool     tile_fallback;
    bool     tile_image;
} music_test_state_t;

void ui_music_test_state(music_test_state_t *out);
#endif

#endif /* CHATBOX_UI_MUSIC_H */
