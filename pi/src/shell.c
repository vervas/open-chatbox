/**
 * shell.c — App shell: registry, focus groups, key routing, switcher glue
 * (docs/player-spec.md §5, §6, §8).
 *
 * Key routing — where each §5 step happens
 * ----------------------------------------
 * The shell wraps every keyboard indev's read callback (shell_read_cb).
 * The hook sees each key as the driver reports it, *before* LVGL
 * dispatches anything, and only acts on press edges:
 *
 *   1. Display locked/off: the key is recorded in the filter and the whole
 *      press is swallowed (LVGL is told the key is released until the
 *      driver releases it). The hook keeps the display awake meanwhile, so
 *      display_sleep wakes on its next check. Nothing reaches an app, not
 *      even a key still held after the wake. The lock overlay is also in
 *      the active group (display_sleep.c) as a second line of defence.
 *   2. Any key: a running switcher animation jumps to its end state first
 *      (switcher_finish_anims), so the key lands in the new state.
 *   3. Esc: filtered (one-shot in every §8.1 context) and swallowed for the
 *      whole press, so LVGL never sees it and can never re-send it as a
 *      held-key repeat. If accepted: switcher open → switcher closes;
 *      else the visible app's on_key(Esc) (chat: closes the emoji panel;
 *      music: closes Now Playing; photos: closes the delete confirm or
 *      the viewer);
 *      else the switcher opens.
 *   4. Other keys pass through to LVGL untouched and reach the focused
 *      object of the active group (the switcher's, else the visible app's).
 *      The switcher and music run every LV_EVENT_KEY they get — including
 *      LVGL's own held-key repeats, which the hook never sees — through
 *      shell_filter_key(). Chat takes keys straight from LVGL, so for chat
 *      the hook records each press edge (never one-shot there, §8.1).
 *      shell_filter_key() also drops a one-shot key that LVGL re-sends
 *      while it is still physically held (see the note there: LVGL 9.2's
 *      first repeat can come later than the 450 ms window).
 *
 * Release reset (§8.1): a real release of a key clears its record
 * (key_filter_release), so every separate press acts and only repeats of a
 * key that is still held meet the 450 ms window. Where the real release
 * comes from depends on the keyboard:
 *
 *   - libinput (Pi) and the test indev: the hook's own release edge. It is
 *     real: the driver holds a key as long as it is down (arrows,
 *     Backspace) or releases it at once and never repeats it (the rest).
 *   - LVGL's SDL driver (desktop): every press — OS auto-repeats included —
 *     arrives as a press + an immediate fake release, and SDL_KEYUP is
 *     ignored. Such a keyboard is marked with
 *     shell_keyboard_use_external_release(). main.c's SDL event watch then
 *     reports each queued press (shell_key_pushed) and each real key-up
 *     (shell_key_released) in SDL's queue order. The watch runs when SDL
 *     queues an event, which can be before LVGL has handled an earlier
 *     press of the same key, so the reset is not applied at the key-up: each
 *     queued press is tagged "follows a key-up" and the reset is applied
 *     when LVGL delivers that press to the hook (ext_take).
 *
 * One key_filter_t records everything, so a held key can't act twice
 * across a context change. Known gap: LVGL's held-key repeats *inside
 * chat* (Pi arrows/Backspace) are not recorded — they never leave chat,
 * because leaving chat takes an Esc press, which is a new press edge.
 *
 * Context changes caused by a key that LVGL is dispatching (Enter on a
 * switcher tile) call lv_indev_wait_release(), so the rest of that press
 * (LVGL repeats, RELEASED/CLICKED) is not delivered to the new group.
 * Changes made from inside the hook don't: the hook is either swallowing
 * that key (Esc) or about to deliver it to the new state on purpose (2).
 *
 * Why the read level: on the Pi, LVGL itself re-sends a held key after
 * 400 ms, so an interception at LV_EVENT_KEY level would see Esc repeats;
 * at read level Esc is one press per press. Every driver (libinput, SDL,
 * the test indev) goes through lv_indev_read → read_cb, so one hook
 * covers them all.
 */

#include "shell.h"
#include "switcher.h"
#include "display_sleep.h"
#include "ui_theme.h"
#include "ui_hints.h"

#include <stdio.h>
#include <string.h>

/* key_filter.h mirrors LVGL's key codes so it can stay LVGL-free. Space is
 * not an LV_KEY_*: SDL delivers it as SDL_TEXTINPUT " " (the driver's
 * keycode_to_ctrl_key maps SDLK_SPACE to nothing), and libinput/xkb as the
 * UTF-8 byte of the keysym, both 32. */
_Static_assert(KF_KEY_ENTER == LV_KEY_ENTER, "KF_KEY_ENTER != LV_KEY_ENTER");
_Static_assert(KF_KEY_ESC   == LV_KEY_ESC,   "KF_KEY_ESC != LV_KEY_ESC");
_Static_assert(KF_KEY_UP    == LV_KEY_UP,    "KF_KEY_UP != LV_KEY_UP");
_Static_assert(KF_KEY_DOWN  == LV_KEY_DOWN,  "KF_KEY_DOWN != LV_KEY_DOWN");
_Static_assert(KF_KEY_RIGHT == LV_KEY_RIGHT, "KF_KEY_RIGHT != LV_KEY_RIGHT");
_Static_assert(KF_KEY_LEFT  == LV_KEY_LEFT,  "KF_KEY_LEFT != LV_KEY_LEFT");
_Static_assert(KF_KEY_SPACE == ' ',          "KF_KEY_SPACE != ' '");
_Static_assert(KF_KEY_DEL   == LV_KEY_DEL,   "KF_KEY_DEL != LV_KEY_DEL");

/* ---- State -------------------------------------------------------------- */

typedef struct {
    const app_t *app;
    lv_obj_t    *root;
    bool         filters_own_keys;
} shell_app_t;

static shell_app_t g_apps[SHELL_MAX_APPS];
static int         g_app_count      = 0;
static int         g_visible        = -1;   /* index into g_apps */
static bool        g_switcher_open  = false;
static bool        g_started        = false;

static key_filter_t g_kf;

/* Peek pills (shell_attach_peek): a host app's pill showing app `src`. */
#define SHELL_MAX_PEEKS 4
typedef struct {
    lv_obj_t *pill;
    int       src;
} shell_peek_t;
static shell_peek_t g_peeks[SHELL_MAX_PEEKS];
static int          g_peek_count = 0;

#ifdef CHATBOX_KEY_HINTS
static lv_timer_t *g_hints_timer = NULL;
static lv_obj_t   *g_hints_bar   = NULL;
static void hints_timer_cb(lv_timer_t *t);
#endif
static void render_peek(int p);

/* Per-keyboard hook state. A handful of slots: there is one keyboard, and
 * a hotplug deletes the old indev (LV_EVENT_DELETE frees its slot) before
 * attaching the new one. */
#define SHELL_MAX_KEYPADS 4
typedef struct {
    lv_indev_t        *indev;
    lv_indev_read_cb_t orig;
    bool               raw_pressed;  /* driver's state on the last read */
    uint32_t           raw_key;
    bool               swallow;      /* current press is hidden from LVGL */
    bool               fresh_edge;   /* this read is the press edge */
    bool               ext_release;  /* releases come from shell_key_released */
} kb_slot_t;

static kb_slot_t g_kb[SHELL_MAX_KEYPADS];

/* True while shell_read_cb runs (see the wait_release note above). */
static bool g_in_read_hook = false;

/* External release source (SDL): presses queued for LVGL, oldest first,
 * each tagged with whether a real key-up of that key came before it. A
 * handful of entries: LVGL handles a queued press within one main-loop
 * pass. Entries older than SHELL_EXT_STALE_MS are presses LVGL never
 * delivered (e.g. dropped by its driver): they are discarded so they can't
 * be matched to a later press. */
#define SHELL_EXT_QUEUE    16
#define SHELL_EXT_STALE_MS 1000u
#define SHELL_EXT_KEYS     128   /* key codes tracked: all the §8.1 keys are < 128 */
typedef struct {
    uint32_t key;
    uint32_t t;
    bool     after_release;
} ext_press_t;

static ext_press_t g_ext_q[SHELL_EXT_QUEUE];
static int         g_ext_n = 0;
static bool        g_ext_down[SHELL_EXT_KEYS];  /* SDL's view: key is down */

/* ---- Helpers ------------------------------------------------------------ */

static int find_app(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < g_app_count; i++) {
        if (strcmp(g_apps[i].app->id, id) == 0) return i;
    }
    return -1;
}

lv_group_t *shell_active_group(void)
{
    if (!g_started) return NULL;
    if (g_switcher_open) return switcher_group();
    if (g_visible >= 0 && g_apps[g_visible].app->group)
        return g_apps[g_visible].app->group();
    return NULL;
}

/* Point every keypad at the active group — wrapped or not, like
 * ui_sidebar_create does — so no keyboard can reach a hidden app (D3). */
static void apply_group(void)
{
    lv_group_t *grp = shell_active_group();
    if (!grp) return;
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        lv_indev_type_t t = lv_indev_get_type(i);
        if (t == LV_INDEV_TYPE_KEYPAD || t == LV_INDEV_TYPE_ENCODER)
            lv_indev_set_group(i, grp);
    }
}

/* The key LVGL is dispatching right now caused a context change: make the
 * rest of that press dead (no repeats, no RELEASED/CLICKED in the new
 * group). Not from inside the hook: see the header comment. */
static void consume_active_press(void)
{
    if (g_in_read_hook) return;
    lv_indev_t *act = lv_indev_active();
    if (act && lv_indev_get_type(act) == LV_INDEV_TYPE_KEYPAD)
        lv_indev_wait_release(act);
}

/* True when keys go straight from LVGL to an app that doesn't filter them
 * itself (chat): the hook records those presses. */
static bool pass_through(void)
{
    if (!g_started || g_switcher_open || g_visible < 0) return false;
    return !g_apps[g_visible].filters_own_keys;
}

/* ---- Public API --------------------------------------------------------- */

void shell_init(void)
{
    memset(g_apps, 0, sizeof(g_apps));
    memset(g_kb, 0, sizeof(g_kb));
    g_app_count     = 0;
    g_visible       = -1;
    g_switcher_open = false;
    g_started       = false;
    g_in_read_hook  = false;
    g_ext_n         = 0;
    memset(g_ext_down, 0, sizeof(g_ext_down));
    memset(g_peeks, 0, sizeof(g_peeks));
    g_peek_count    = 0;
    key_filter_init(&g_kf);
}

void shell_register_app(const app_t *app, bool filters_own_keys)
{
    if (!app || g_app_count >= SHELL_MAX_APPS) {
        fprintf(stderr, "shell: cannot register app %s\n", app ? app->id : "(null)");
        return;
    }
    g_apps[g_app_count].app              = app;
    g_apps[g_app_count].root             = NULL;
    g_apps[g_app_count].filters_own_keys = filters_own_keys;
    g_app_count++;
}

lv_group_t *shell_suspend_default_group(void)
{
    lv_group_t *saved = lv_group_get_default();
    lv_group_set_default(NULL);
    return saved;
}

void shell_restore_default_group(lv_group_t *saved)
{
    lv_group_set_default(saved);
}

void shell_start(const char *boot_app_id)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Each app builds with no default group, so nothing it creates is
     * auto-added to another app's group. An app that sets its own default
     * (chat, via ui_sidebar_create) keeps it: chat code relies on
     * lv_group_get_default() being chat's group. */
    /* Roots are hidden only after every app is built: LVGL's group focus
     * skips objects under a hidden parent, so a group filled while its root
     * is hidden would start with nothing focused. */
    lv_group_t *keep_default = lv_group_get_default();
    for (int i = 0; i < g_app_count; i++) {
        lv_obj_t *root = lv_obj_create(scr);
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, UI_W, UI_APP_H);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        g_apps[i].root = root;

        lv_group_set_default(NULL);
        g_apps[i].app->create(root);
        if (lv_group_get_default()) keep_default = lv_group_get_default();
    }
    for (int i = 0; i < g_app_count; i++)
        lv_obj_add_flag(g_apps[i].root, LV_OBJ_FLAG_HIDDEN);

    /* The switcher goes last so it draws above every app root. */
    switcher_tile_info_t tiles[SWITCHER_MAX_TILES];
    int n = 0;
    for (int i = 0; i < g_app_count && n < SWITCHER_MAX_TILES; i++, n++) {
        const app_t *app = g_apps[i].app;
        tiles[n].id    = app->id;
        tiles[n].label = app->label ? app->label : app->id;
        tiles[n].look  = app->look;
    }
    lv_group_set_default(NULL);
    switcher_create(scr, tiles, n);
#ifdef CHATBOX_KEY_HINTS
    /* Above everything, the switcher included: it shows the switcher's
     * hints while that is open. */
    g_hints_bar = ui_hints_create(scr);
    g_hints_timer = lv_timer_create(hints_timer_cb, 100, NULL);
#endif
    lv_group_set_default(keep_default);

    for (int i = 0; i < g_peek_count; i++) render_peek(i);

    int boot = find_app(boot_app_id);
    if (boot < 0) {
        fprintf(stderr, "shell: unknown boot app '%s', using %s\n",
                boot_app_id ? boot_app_id : "(null)",
                g_app_count ? g_apps[0].app->id : "(none)");
        boot = 0;
    }
    g_started = true;
    if (g_app_count == 0) return;

    g_visible = boot;
    lv_obj_clear_flag(g_apps[boot].root, LV_OBJ_FLAG_HIDDEN);
    if (g_apps[boot].app->on_show) g_apps[boot].app->on_show();
    apply_group();
    fprintf(stderr, "shell: started, visible app %s\n", g_apps[boot].app->id);
}

const app_t *shell_visible_app(void)
{
    return g_visible >= 0 ? g_apps[g_visible].app : NULL;
}

bool shell_switcher_is_open(void)
{
    return g_switcher_open;
}

void shell_switch_to(const char *id)
{
    int idx = find_app(id);
    if (idx < 0 || !g_started) return;

    if (idx != g_visible) {
        if (g_visible >= 0) {
            const app_t *old = g_apps[g_visible].app;
            if (old->on_hide) old->on_hide();
            lv_obj_add_flag(g_apps[g_visible].root, LV_OBJ_FLAG_HIDDEN);
        }
        g_visible = idx;
        lv_obj_clear_flag(g_apps[idx].root, LV_OBJ_FLAG_HIDDEN);
        if (g_apps[idx].app->on_show) g_apps[idx].app->on_show();
        fprintf(stderr, "shell: visible app %s\n", id);
    }

    g_switcher_open = false;
    switcher_close();
    apply_group();
    consume_active_press();
}

/* Peek pill `p`: its source app fills it, or says there's nothing to show. */
static void render_peek(int p)
{
    const app_t *app = g_apps[g_peeks[p].src].app;
    lv_obj_t *pill = g_peeks[p].pill;
    if (!app->peek_render) return;
    lv_group_t *saved = shell_suspend_default_group();
    bool show = app->peek_render(pill);
    shell_restore_default_group(saved);
    if (show) lv_obj_clear_flag(pill, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
}

#ifdef CHATBOX_KEY_HINTS
/* The hints of whatever is on screen. A list is static, so comparing
 * pointers (ui_hints_set) makes this cheap enough to poll. */
static void hints_timer_cb(lv_timer_t *t)
{
    (void)t;
    int n = 0;
    const ui_hint_t *h = NULL;
    if (g_switcher_open) h = switcher_hints(&n);
    else if (g_visible >= 0 && g_apps[g_visible].app->hints) h = g_apps[g_visible].app->hints(&n);
    ui_hints_set(h, h ? n : 0);
}
#endif

/* Live tiles (T9): app `i` fills its tile's face. With no default group,
 * so nothing an app builds there can join a focus group. */
static void render_tile(int i)
{
    const app_t *app = g_apps[i].app;
    if (!app->tile_render) return;
    lv_obj_t *face = switcher_tile_face(app->id);
    if (!face) return;
    lv_group_t *saved = shell_suspend_default_group();
    app->tile_render(face);
    shell_restore_default_group(saved);
}

void shell_refresh_tile(const char *id)
{
    int i = find_app(id);
    if (i < 0) return;
    for (int p = 0; p < g_peek_count; p++)
        if (g_peeks[p].src == i) render_peek(p);
    /* Tiles only while the switcher is open: it renders every tile when it
     * opens. */
    if (g_switcher_open) render_tile(i);
}

void shell_attach_peek(lv_obj_t *pill, const char *src_id)
{
    int src = find_app(src_id);
    if (!pill || src < 0 || g_peek_count >= SHELL_MAX_PEEKS) return;
    g_peeks[g_peek_count].pill = pill;
    g_peeks[g_peek_count].src  = src;
    g_peek_count++;
    /* Rendered at the end of shell_start, once every app exists. */
    if (g_started) render_peek(g_peek_count - 1);
}

void shell_open_switcher(void)
{
    if (!g_started || g_switcher_open || g_visible < 0) return;
    g_switcher_open = true;
    for (int i = 0; i < g_app_count; i++) render_tile(i);
    switcher_open(g_apps[g_visible].app->id);
#ifdef CHATBOX_KEY_HINTS
    /* switcher_open brings the switcher to the front: the bar goes back on
     * top of it (it shows the switcher's hints). */
    if (g_hints_bar) lv_obj_move_foreground(g_hints_bar);
#endif
    apply_group();
    consume_active_press();
}

void shell_close_switcher(void)
{
    if (!g_switcher_open) return;
    g_switcher_open = false;
    switcher_close();
    apply_group();
    consume_active_press();
}

void shell_consume_press(void)
{
    consume_active_press();
}

static kb_slot_t *find_slot(lv_indev_t *indev);

/* True when the LV_EVENT_KEY being dispatched is LVGL re-sending a key
 * that is still held on a hooked keyboard — not the press itself. */
static bool is_lvgl_repeat(uint32_t key)
{
    lv_indev_t *act = lv_indev_active();
    kb_slot_t *s = act ? find_slot(act) : NULL;
    return s && s->raw_pressed && s->raw_key == key && !s->fresh_edge;
}

bool shell_filter_key(uint32_t key, key_ctx_t ctx)
{
    bool one_shot = key_filter_is_one_shot(key, ctx);
    bool accept = key_filter_accept(&g_kf, key, lv_tick_get(), one_shot);

    /* D2 says a held one-shot key fires exactly once, assuming LVGL's
     * repeats come at +400 ms and then every 200 ms. In LVGL 9.2 the first
     * repeat comes one or two indev reads (33 ms each) after the 400 ms
     * long-press mark — up to ~470 ms, past the 450 ms window — and then
     * every 100 ms (LV_INDEV_DEF_LONG_PRESS_REP_TIME is hard-coded in
     * lv_indev.c; lv_conf.h's value is unused). So a one-shot key event
     * that the hook can see is still physically held is dropped too. The
     * window rule above still covers SDL, whose OS repeats are presses
     * with no key-up between them (so nothing resets the record). */
    if (accept && one_shot && is_lvgl_repeat(key)) accept = false;
    return accept;
}

/* §5 steps 3–5 for an Esc that the filter accepted. */
static void route_esc(void)
{
    if (g_switcher_open) {
        switcher_handle_key(LV_KEY_ESC);
        return;
    }
    if (g_visible >= 0) {
        const app_t *app = g_apps[g_visible].app;
        if (app->on_key && app->on_key(LV_KEY_ESC)) return;
    }
    shell_open_switcher();
}

void shell_handle_esc(void)
{
    if (!g_started || display_sleep_is_locked()) return;
    switcher_finish_anims();
    /* Esc is one-shot in every §8.1 context. */
    if (key_filter_accept(&g_kf, LV_KEY_ESC, lv_tick_get(), true))
        route_esc();
}

/* ---- External release source (SDL) ------------------------------------- */

void shell_key_pushed(uint32_t key)
{
    if (key >= SHELL_EXT_KEYS) return;
    if (g_ext_n == SHELL_EXT_QUEUE) {
        /* Full (LVGL isn't draining): forget the oldest. */
        memmove(&g_ext_q[0], &g_ext_q[1], sizeof(g_ext_q[0]) * (SHELL_EXT_QUEUE - 1));
        g_ext_n--;
    }
    g_ext_q[g_ext_n].key           = key;
    g_ext_q[g_ext_n].t             = lv_tick_get();
    g_ext_q[g_ext_n].after_release = !g_ext_down[key];
    g_ext_n++;
    g_ext_down[key] = true;
}

void shell_key_released(uint32_t key)
{
    if (key < SHELL_EXT_KEYS) g_ext_down[key] = false;
}

/* LVGL is delivering a press of `key` from an external-release keyboard:
 * take the oldest queued press of that key. Returns true when it followed
 * a real key-up. A press the watch never reported counts as not following
 * one (the 450 ms window then decides, as before the release reset). */
static bool ext_take(uint32_t key, uint32_t now)
{
    /* Queue order is time order: stale entries are at the head. */
    int stale = 0;
    while (stale < g_ext_n && (uint32_t)(now - g_ext_q[stale].t) > SHELL_EXT_STALE_MS)
        stale++;
    if (stale) {
        memmove(&g_ext_q[0], &g_ext_q[stale], sizeof(g_ext_q[0]) * (size_t)(g_ext_n - stale));
        g_ext_n -= stale;
    }
    for (int i = 0; i < g_ext_n; i++) {
        if (g_ext_q[i].key != key) continue;
        bool after_release = g_ext_q[i].after_release;
        memmove(&g_ext_q[i], &g_ext_q[i + 1], sizeof(g_ext_q[0]) * (size_t)(g_ext_n - i - 1));
        g_ext_n--;
        return after_release;
    }
    return false;
}

/* ---- Keyboard read hook ------------------------------------------------- */

static kb_slot_t *find_slot(lv_indev_t *indev)
{
    for (int i = 0; i < SHELL_MAX_KEYPADS; i++) {
        if (g_kb[i].indev == indev) return &g_kb[i];
    }
    return NULL;
}

static void shell_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    kb_slot_t *s = find_slot(indev);
    if (!s || !s->orig) return;

    s->orig(indev, data);

    bool pressed = (data->state == LV_INDEV_STATE_PRESSED);
    /* A press edge: a new press, or the driver switching keys while one is
     * down (a second key pressed on top of the first). */
    bool edge = pressed && (!s->raw_pressed || data->key != s->raw_key);
    /* A release edge: the driver reports no key down any more. (A switch
     * to another key is not a release of the first: it may still be down.) */
    bool released = !pressed && s->raw_pressed;
    uint32_t released_key = s->raw_key;
    s->raw_pressed = pressed;
    if (pressed) s->raw_key = data->key;
    s->fresh_edge = edge;

    /* Release reset (§8.1), for keyboards whose release edges are real. */
    if (released && !s->ext_release)
        key_filter_release(&g_kf, released_key);

    if (edge) {
        uint32_t key = data->key;
        uint32_t now = lv_tick_get();
        s->swallow = false;
        g_in_read_hook = true;

        /* Release reset (§8.1) for an external-release keyboard (SDL): this
         * press came after a real key-up, so the key's record goes before
         * anything below filters or records it. */
        if (s->ext_release && ext_take(key, now))
            key_filter_release(&g_kf, key);

        if (display_sleep_is_locked()) {
            /* §5 step 1: swallowed, only the time is recorded. */
            key_filter_record(&g_kf, key, now);
            s->swallow = true;
        } else if (g_started) {
            switcher_finish_anims();
            if (key == LV_KEY_ESC) {
                s->swallow = true;
                /* Esc is one-shot in every §8.1 context. */
                if (key_filter_accept(&g_kf, key, now, true))
                    route_esc();
            } else if (pass_through()) {
                key_filter_record(&g_kf, key, now);
            }
        }

        g_in_read_hook = false;
    }

    if (s->swallow) {
        if (pressed) {
            /* Hide the press from LVGL, but keep the display awake the way
             * LVGL would for a real press. */
            lv_display_trigger_activity(lv_indev_get_display(indev));
            data->state = LV_INDEV_STATE_RELEASED;
        } else {
            s->swallow = false;
        }
    }
}

void shell_keyboard_use_external_release(lv_indev_t *indev, bool on)
{
    kb_slot_t *s = find_slot(indev);
    if (s) s->ext_release = on;
}

static void kb_delete_cb(lv_event_t *e)
{
    kb_slot_t *s = find_slot(lv_event_get_user_data(e));
    if (s) memset(s, 0, sizeof(*s));
}

void shell_attach_keyboard(lv_indev_t *indev)
{
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) return;

    kb_slot_t *s = find_slot(indev);
    if (!s) {
        s = find_slot(NULL);
        if (!s) {
            fprintf(stderr, "shell: too many keyboards, not hooking %p\n", (void *)indev);
            return;
        }
        memset(s, 0, sizeof(*s));
        s->indev = indev;
        s->orig  = lv_indev_get_read_cb(indev);
        lv_indev_set_read_cb(indev, shell_read_cb);
        lv_indev_add_event_cb(indev, kb_delete_cb, LV_EVENT_DELETE, indev);
    }

    lv_group_t *grp = shell_active_group();
    if (grp) lv_indev_set_group(indev, grp);
}

#ifdef CHATBOX_TESTING
void shell_deinit(void)
{
#ifdef CHATBOX_KEY_HINTS
    if (g_hints_timer) lv_timer_delete(g_hints_timer);
    if (g_hints_bar) lv_obj_delete(g_hints_bar);
    g_hints_timer = NULL;
    g_hints_bar = NULL;
#endif
    switcher_destroy();
    lv_group_t *def = lv_group_get_default();
    for (int i = 0; i < g_app_count; i++) {
        lv_group_t *g = g_apps[i].app->group ? g_apps[i].app->group() : NULL;
        /* Empty the group first so deleting the widgets can't fire focus
         * callbacks (sidebar row_focus_cb) on half-deleted objects. */
        if (g) lv_group_remove_all_objs(g);
        if (g_apps[i].root) lv_obj_delete(g_apps[i].root);
        if (g && g != def) lv_group_delete(g);
    }
    /* Keep the keyboard hooks: the indevs outlive a test's shell. */
    kb_slot_t kb[SHELL_MAX_KEYPADS];
    memcpy(kb, g_kb, sizeof(kb));
    shell_init();
    memcpy(g_kb, kb, sizeof(kb));
}

key_filter_t *shell_get_key_filter(void)
{
    return &g_kf;
}
#endif
