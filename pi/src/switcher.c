/**
 * switcher.c — Full-screen app switcher overlay (docs/player-spec.md §6, §8.5).
 *
 * The sticker kit's Home (ui_theme.h): the page colour, "Hi there!" and a
 * clock at the top, one big sticker per app (Music, Chat, Photos, left to
 * right) in the app's colour with a thick ink ring. Two tiles are squares;
 * three stand upright, 360×520, so the faces keep room for a 256 px cover. The focused tile gets a thicker ring
 * and a big solid ink shadow; the visible app's tile wears a yellow "Now"
 * sticker.
 *
 * Tile anatomy (every tile, whatever its state):
 *   tile      the coloured sticker. No padding, and it never gets a border:
 *             LVGL insets a parent's content box by its border width, so a
 *             border would shift everything in it. The ring is the tile's
 *             outline and the shadow is drawn outside: neither moves a thing.
 *   face      the live-tile area (T9): a fixed box above the caption band
 *             that the app fills through app->tile_render(). The shell
 *             renders the faces when the switcher opens and when an app
 *             reports a change while it is open (shell_refresh_tile).
 *   caption   the app name, bottom-left, at a fixed offset.
 *   Now tag   top-right.
 * So the caption and the face sit at identical positions in every tile,
 * focused or not, current or not.
 *
 * Keys (already through the D2 filter, KEY_CTX_SWITCHER):
 *   ← / →   focus the left / right tile; shake at the edges
 *   ↑ / ↓   bounce (vertical shake)
 *   Enter   open the focused app with a zoom; on the Now tile just close
 *   Esc     close (routed here by the shell)
 *   other   ignored. Tab has nowhere to go: the group holds one key sink.
 *
 * Rendering cost (the Pi draws a 1280×800 framebuffer in software):
 *   - The overlay is an opaque child of the screen, drawn above the app
 *     roots, so LVGL's cover check skips redrawing the app underneath.
 *   - No opacity animation anywhere. The fly-in and the shakes move tiles
 *     with translate_x/y; the zoom grows one tile's geometry. Each frame
 *     only redraws the tiles' old and new areas. The shadows are solid
 *     (width 2), not blurred.
 *
 * Animations never block input: the shell calls switcher_finish_anims()
 * on every key press edge before the key is dispatched, which jumps any
 * animation to its end state (a zoom completes its app switch), so the
 * key is handled in the new state.
 */

#include "switcher.h"
#include "shell.h"
#include "ui_theme.h"
#include "ui_hints.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Look ---------------------------------------------------------------- */

#define SW_RING            5   /* every tile's ink ring */
#define SW_RING_FOCUS      7
#define SW_SHADOW_FOCUS   14

#define SW_MARGIN_X      100
#define SW_TILE_TOP      170
#define SW_TILE_GAP       56
#define SW_TILE_MAX      520
#define SW_MARGIN_X_3     60   /* three tiles (Music, Chat, Photos) */
#define SW_TILE_GAP_3     40
#define SW_PAD            32   /* face and caption inset from the tile edge */
#define SW_CAP_GAP        12   /* between the face and the caption */

/* ---- Timing (ms) --------------------------------------------------------- */

#define SW_FLYIN_MS      190   /* per tile; staggered, ~250 ms in total */
#define SW_FLYIN_STAGGER  60
#define SW_FLYIN_DX      200   /* tiles start this far right */
#define SW_ZOOM_MS       200
#define SW_SHAKE_MS      240
#define SW_SHAKE_AMP      18

/* ---- State --------------------------------------------------------------- */

typedef struct {
    const char *id;
    lv_obj_t   *tile;
    lv_obj_t   *face;        /* live-tile area, filled by the app */
    lv_obj_t   *caption;
    lv_obj_t   *now_tag;
    int32_t     x, y, w, h;      /* resting geometry */
} sw_tile_t;

static lv_obj_t   *g_root      = NULL;
static lv_obj_t   *g_sink      = NULL;
static lv_obj_t   *g_clock     = NULL;
static lv_group_t *g_group     = NULL;
static lv_timer_t *g_clock_tmr = NULL;
static sw_tile_t   g_tiles[SWITCHER_MAX_TILES];
static int         g_count     = 0;
static int         g_focus     = 0;
static int         g_current   = 0;
static int         g_zoom_idx  = -1;      /* tile zooming, -1 = none */
static int         g_shakes    = 0;       /* test counters */
static int         g_bounces   = 0;

static void sink_key_cb(lv_event_t *e);

/* ---- Helpers ------------------------------------------------------------- */

static int tile_index(const char *id)
{
    for (int i = 0; i < g_count; i++) {
        if (id && strcmp(g_tiles[i].id, id) == 0) return i;
    }
    return -1;
}

static void update_clock(void)
{
    if (!g_clock) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    lv_label_set_text_fmt(g_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_clock();
}

static void apply_marks(void)
{
    for (int i = 0; i < g_count; i++) {
        lv_obj_t *tile = g_tiles[i].tile;
        bool focus = (i == g_focus);
        /* Neither mark touches the tile's content box (see the header). */
        ui_sticker(tile, focus ? SW_RING_FOCUS : SW_RING, focus ? SW_SHADOW_FOCUS : 0);
        if (i == g_current) lv_obj_clear_flag(g_tiles[i].now_tag, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(g_tiles[i].now_tag, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- Animations ------------------------------------------------------------
 * One exec callback per kind, so each kind can be deleted on its own. */

static void flyin_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_x(var, v, 0);
}

/* v: 0..1000 progress → a decaying sine, 1.5 periods. */
static int32_t shake_offset(int32_t v, int32_t amp)
{
    int32_t s = lv_trigo_sin((int16_t)((v * 540) / 1000));
    return (s * amp * (1000 - v)) / (1000 * LV_TRIGO_SIN_MAX);
}

static void shake_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_x(var, shake_offset(v, SW_SHAKE_AMP), 0);
}

static void bounce_up_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_y(var, -shake_offset(v, SW_SHAKE_AMP), 0);
}

static void bounce_down_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_y(var, shake_offset(v, SW_SHAKE_AMP), 0);
}

/* v: 0..1024 progress from the tile's resting rect to the full screen. */
static void zoom_exec(void *var, int32_t v)
{
    sw_tile_t *t = NULL;
    for (int i = 0; i < g_count; i++) {
        if (g_tiles[i].tile == var) t = &g_tiles[i];
    }
    if (!t) return;
    int32_t sw = lv_obj_get_width(g_root);
    int32_t sh = lv_obj_get_height(g_root);
    lv_obj_set_pos(var, t->x - (t->x * v) / 1024, t->y - (t->y * v) / 1024);
    lv_obj_set_size(var, t->w + ((sw - t->w) * v) / 1024,
                         t->h + ((sh - t->h) * v) / 1024);
}

static void start_anim(lv_obj_t *tile, lv_anim_exec_xcb_t exec, int32_t from,
                       int32_t to, uint32_t ms, uint32_t delay,
                       lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tile);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

/* Stop everything and put every tile back at rest. No completion runs. */
static void stop_all_anims(void)
{
    for (int i = 0; i < g_count; i++) {
        sw_tile_t *t = &g_tiles[i];
        lv_anim_delete(t->tile, NULL);
        lv_obj_set_style_translate_x(t->tile, 0, 0);
        lv_obj_set_style_translate_y(t->tile, 0, 0);
        lv_obj_set_pos(t->tile, t->x, t->y);
        lv_obj_set_size(t->tile, t->w, t->h);
        lv_obj_set_style_radius(t->tile, UI_RADIUS_BIG, 0);   /* the zoom squares it */
    }
    g_zoom_idx = -1;
}

static void zoom_completed(void)
{
    int idx = g_zoom_idx;
    g_zoom_idx = -1;
    if (idx < 0 || idx >= g_count) return;
    /* Switches app and closes the switcher (which resets the tiles). */
    shell_switch_to(g_tiles[idx].id);
}

static void zoom_ready_cb(lv_anim_t *a)
{
    (void)a;
    zoom_completed();
}

static void shake(int idx)
{
    lv_obj_t *tile = g_tiles[idx].tile;
    lv_anim_delete(tile, flyin_exec);
    lv_obj_set_style_translate_x(tile, 0, 0);
    start_anim(tile, shake_exec, 0, 1000, SW_SHAKE_MS, 0, lv_anim_path_linear, NULL);
    g_shakes++;
}

static void bounce(int idx, bool up)
{
    lv_obj_t *tile = g_tiles[idx].tile;
    lv_anim_delete(tile, bounce_up_exec);
    lv_anim_delete(tile, bounce_down_exec);
    start_anim(tile, up ? bounce_up_exec : bounce_down_exec, 0, 1000,
               SW_SHAKE_MS, 0, lv_anim_path_linear, NULL);
    g_bounces++;
}

/* ---- Public API ------------------------------------------------------------ */

void switcher_create(lv_obj_t *parent, const switcher_tile_info_t *tiles, int count)
{
    if (count > SWITCHER_MAX_TILES) count = SWITCHER_MAX_TILES;
    memset(g_tiles, 0, sizeof(g_tiles));
    g_count    = count;
    g_focus    = 0;
    g_current  = 0;
    g_zoom_idx = -1;
    g_shakes   = 0;
    g_bounces  = 0;

    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_root, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    /* Clickable so a stray tap/click (Pi touchscreen, SDL mouse) lands on
     * the overlay instead of a chat widget underneath. It does nothing. */
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hello = lv_label_create(g_root);
    lv_label_set_text(hello, "Hi there!");
    lv_obj_set_style_text_font(hello, &font_title_48, 0);
    lv_obj_set_style_text_color(hello, UI_C_INK, 0);
    lv_obj_set_pos(hello, SW_MARGIN_X, 56);

    g_clock = lv_label_create(g_root);
    lv_obj_set_style_text_font(g_clock, &font_title_48, 0);
    lv_obj_set_style_text_color(g_clock, UI_C_INK_2, 0);
    lv_obj_align(g_clock, LV_ALIGN_TOP_RIGHT, -SW_MARGIN_X, 56);
    update_clock();

    /* Two tiles are squares; three or more stand upright (as tall as two
     * square tiles, narrower, with a smaller margin and gap) so the
     * album cover and the photo still fit their faces. */
    int32_t sw = lv_display_get_horizontal_resolution(NULL);
    int32_t sh = lv_display_get_vertical_resolution(NULL);
    bool    tall = count >= 3;
    int32_t margin = tall ? SW_MARGIN_X_3 : SW_MARGIN_X;
    int32_t gap = tall ? SW_TILE_GAP_3 : SW_TILE_GAP;
    int32_t hmax = sh - UI_HINTS_H - SW_TILE_TOP - 70;
    int32_t tw = count > 0 ? (sw - 2 * margin - (count - 1) * gap) / count : 0;
    if (tw > SW_TILE_MAX) tw = SW_TILE_MAX;
    if (tw > hmax) tw = hmax;
    int32_t th = tall ? (SW_TILE_MAX < hmax ? SW_TILE_MAX : hmax) : tw;
    int32_t total = count * tw + (count - 1) * gap;
    int32_t x0 = (sw - total) / 2;

    for (int i = 0; i < count; i++) {
        sw_tile_t *t = &g_tiles[i];
        t->id = tiles[i].id;
        t->x  = x0 + i * (tw + gap);
        t->y  = SW_TILE_TOP;
        t->w  = tw;
        t->h  = th;

        ui_app_t app = tiles[i].look;

        lv_obj_t *tile = lv_obj_create(g_root);
        lv_obj_remove_style_all(tile);
        lv_obj_set_pos(tile, t->x, t->y);
        lv_obj_set_size(tile, t->w, t->h);
        lv_obj_set_style_bg_color(tile, ui_app_color(app), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, UI_RADIUS_BIG, 0);
        ui_sticker(tile, SW_RING, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        t->tile = tile;

        int32_t cap_h = lv_font_get_line_height(&font_title_48);

        /* Live-tile area. Empty until the app renders into it. */
        lv_obj_t *face = lv_obj_create(tile);
        lv_obj_remove_style_all(face);
        lv_obj_set_pos(face, SW_PAD, SW_PAD);
        lv_obj_set_size(face, t->w - 2 * SW_PAD, t->h - 2 * SW_PAD - cap_h - SW_CAP_GAP);
        lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        t->face = face;

        lv_obj_t *cap = lv_label_create(tile);
        lv_label_set_text(cap, tiles[i].label);
        lv_obj_set_style_text_font(cap, &font_title_48, 0);
        lv_obj_set_style_text_color(cap, ui_on_app(app), 0);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_LEFT, SW_PAD, -SW_PAD);
        t->caption = cap;

        /* The "Now" sticker: yellow, ink ring, a tick and the word. */
        lv_obj_t *tag = ui_obj(tile);
        lv_obj_set_size(tag, LV_SIZE_CONTENT, 46);
        lv_obj_set_style_radius(tag, 23, 0);
        lv_obj_set_style_bg_color(tag, UI_C_SUN, 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        ui_sticker(tag, 3, 0);
        lv_obj_set_style_pad_hor(tag, 16, 0);
        lv_obj_set_style_pad_column(tag, 8, 0);
        lv_obj_set_flex_flow(tag, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tag, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -SW_PAD + 8, SW_PAD - 8);
        lv_obj_t *tick = ui_label(tag, &lv_font_montserrat_24, UI_C_INK);
        lv_label_set_text(tick, LV_SYMBOL_OK);
        lv_obj_t *tag_lbl = ui_label(tag, &font_title_28, UI_C_INK);
        lv_label_set_text(tag_lbl, "Now");
        lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
        t->now_tag = tag;
    }

    /* The switcher's group holds one invisible key sink: focus between
     * tiles is ours, and Tab/LV_KEY_NEXT has nowhere to go (D3). */
    g_group = lv_group_create();
    g_sink = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_sink);
    lv_obj_set_size(g_sink, 1, 1);
    lv_obj_clear_flag(g_sink, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_sink, sink_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g_group, g_sink);
}

void switcher_destroy(void)
{
    if (g_clock_tmr) {
        lv_timer_delete(g_clock_tmr);
        g_clock_tmr = NULL;
    }
    if (g_root) {
        stop_all_anims();
        if (g_group) lv_group_remove_all_objs(g_group);
        lv_obj_delete(g_root);
    }
    if (g_group) lv_group_delete(g_group);
    g_root = g_sink = g_clock = NULL;
    g_group = NULL;
    g_count = 0;
}

lv_group_t *switcher_group(void)
{
    return g_group;
}

lv_obj_t *switcher_tile_face(const char *id)
{
    int i = tile_index(id);
    return i >= 0 ? g_tiles[i].face : NULL;
}

void switcher_open(const char *current_id)
{
    if (!g_root) return;
    stop_all_anims();

    int cur = tile_index(current_id);
    g_current = cur >= 0 ? cur : 0;
    g_focus   = g_current;
    apply_marks();
    update_clock();
    if (!g_clock_tmr) g_clock_tmr = lv_timer_create(clock_timer_cb, 1000, NULL);

    lv_obj_move_foreground(g_root);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    /* Explicitly: the sink was added while the overlay was hidden, and
     * LVGL's auto-focus skips hidden objects. */
    lv_group_focus_obj(g_sink);

    /* Fly-in: tiles slide in from the right, staggered left to right. */
    for (int i = 0; i < g_count; i++) {
        start_anim(g_tiles[i].tile, flyin_exec, SW_FLYIN_DX, 0, SW_FLYIN_MS,
                   (uint32_t)i * SW_FLYIN_STAGGER, lv_anim_path_ease_out, NULL);
    }
}

void switcher_close(void)
{
    if (!g_root) return;
    stop_all_anims();
    if (g_clock_tmr) {
        lv_timer_delete(g_clock_tmr);
        g_clock_tmr = NULL;
    }
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}

void switcher_finish_anims(void)
{
    if (!g_root || lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN)) return;
    if (g_zoom_idx >= 0) {
        lv_anim_delete(g_tiles[g_zoom_idx].tile, zoom_exec);
        zoom_completed();
        return;
    }
    stop_all_anims();
}

bool switcher_handle_key(uint32_t key)
{
    if (!g_root || g_count == 0) return false;

    switch (key) {
    case LV_KEY_LEFT:
        if (g_focus > 0) { g_focus--; apply_marks(); }
        else shake(g_focus);
        return true;
    case LV_KEY_RIGHT:
        if (g_focus < g_count - 1) { g_focus++; apply_marks(); }
        else shake(g_focus);
        return true;
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        bounce(g_focus, key == LV_KEY_UP);
        return true;
    case LV_KEY_ENTER:
        if (g_focus == g_current) {
            shell_close_switcher();
        } else {
            /* Zoom the picked tile to full screen, then switch. The rest of
             * this Enter press must not reach the app we switch to. */
            shell_consume_press();
            stop_all_anims();
            g_zoom_idx = g_focus;
            lv_obj_t *tile = g_tiles[g_focus].tile;
            ui_sticker(tile, 0, 0);
            lv_obj_set_style_radius(tile, 0, 0);
            lv_obj_move_foreground(tile);
            start_anim(tile, zoom_exec, 0, 1024, SW_ZOOM_MS, 0,
                       lv_anim_path_ease_out, zoom_ready_cb);
        }
        return true;
    case LV_KEY_ESC:
        shell_close_switcher();
        return true;
    default:
        return false;   /* Space, PgUp/PgDn, Win, Tab, letters…: ignored */
    }
}

/* ---- Key hints (CHATBOX_KEY_HINTS) ---------------------------------------- */

static const ui_hint_t k_hints[] = {
    { { UI_KEY_LEFT, UI_KEY_RIGHT }, "choose", false },
    { { "Enter" }, "open", false },
    { { "Esc" }, "back", true },
};

const ui_hint_t *switcher_hints(int *n)
{
    *n = (int)(sizeof(k_hints) / sizeof(k_hints[0]));
    return k_hints;
}

/* ---- Key sink ---------------------------------------------------------------- */

static void sink_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);

    /* Normally Esc never gets here (the shell's read hook takes it). */
    if (key == LV_KEY_ESC) {
        shell_handle_esc();
        return;
    }

    /* A key that reached us without a press edge (e.g. sent straight to the
     * group) still jumps animations first. If that completed a zoom, the key
     * belongs to the app we just switched to. */
    switcher_finish_anims();
    if (!shell_switcher_is_open()) {
        lv_group_t *g = shell_active_group();
        if (g && g != g_group) lv_group_send_data(g, key);
        return;
    }

    if (!shell_filter_key(key, KEY_CTX_SWITCHER)) return;
    switcher_handle_key(key);
}

#ifdef CHATBOX_TESTING
lv_obj_t *switcher_get_root(void)         { return g_root; }
lv_obj_t *switcher_get_tile(int idx)      { return (idx >= 0 && idx < g_count) ? g_tiles[idx].tile : NULL; }
lv_obj_t *switcher_get_now_tag(int idx)   { return (idx >= 0 && idx < g_count) ? g_tiles[idx].now_tag : NULL; }
lv_obj_t *switcher_get_caption(int idx)   { return (idx >= 0 && idx < g_count) ? g_tiles[idx].caption : NULL; }
bool switcher_clock_running(void)         { return g_clock_tmr != NULL; }
int  switcher_get_focus(void)             { return g_focus; }
int  switcher_get_current(void)           { return g_current; }
int  switcher_get_shake_count(void)       { return g_shakes; }
int  switcher_get_bounce_count(void)      { return g_bounces; }
bool switcher_is_zooming(void)            { return g_zoom_idx >= 0; }
bool switcher_is_animating(void)
{
    for (int i = 0; i < g_count; i++) {
        if (lv_anim_get(g_tiles[i].tile, NULL)) return true;
    }
    return false;
}
#endif
