/**
 * app_chat.c — Chat behind the app contract (docs/player-spec.md §5, §8.6).
 *
 * This file only puts the existing chat UI into an app root: the sidebar
 * (ui_sidebar.c) on the left and the content panel on the right, exactly
 * the layout main.c used to build on the screen. The Matrix logic (history,
 * sync, the panel cache, show_chat) stays in main.c, which gets the content
 * panel from app_chat_content_panel().
 *
 * Chat's group is the one ui_sidebar_create() makes and sets as LVGL's
 * default. The shell keeps it as the default, so ui_chat.c, ui_sidebar.c
 * and display_sleep.c's lv_group_get_default() calls still mean "chat's
 * group", and chat code is unchanged.
 *
 * Keys come straight from LVGL (chat doesn't filter them; the shell records
 * them). The only key the shell asks about is Esc: with the emoji panel
 * open, Esc closes it and chat consumes it; otherwise the shell opens the
 * switcher.
 *
 * Live tile (T9, §6): the number of contacts with unread messages and the
 * newest sender's avatar, or a speech-bubble icon when nobody is unread.
 * It reads the sidebar's unread state — the same record the unread dots
 * show (set by main.c's process_sync_payload, cleared when a contact is
 * opened) — so the tile and the dots can't disagree. The sidebar tells us
 * about every change and we pass it on to the shell, which re-renders the
 * tile only while the switcher is open.
 */

#include "app.h"
#include "shell.h"
#include "ui_sidebar.h"
#include "ui_chat.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>

#define TILE_AVATAR   128   /* the avatar PNGs' own size: drawn unscaled */
#define TILE_INSET      6   /* the avatar's ring is drawn outside its box */
#define TILE_BUBBLE_W 220
#define TILE_BUBBLE_H 156
#define TILE_INK      UI_C_ON_CHAT     /* text on the Chat tile */
#define PEEK_AVATAR    36
#define SHELF_GAP      12   /* around the contacts' shelf */

static const config_t *g_cfg = NULL;
static void (*g_on_select)(int contact_idx) = NULL;

static lv_obj_t   *g_root          = NULL;
static lv_obj_t   *g_sidebar_panel = NULL;
static lv_obj_t   *g_content_panel = NULL;
static lv_group_t *g_group         = NULL;
static lv_obj_t   *g_peek          = NULL;   /* music's news, in our header */
static lv_obj_t   *g_nocontacts     = NULL;   /* "No contacts yet" (none in the config) */

static lv_obj_t *tile_obj(lv_obj_t *parent);
static lv_obj_t *bubble_build(lv_obj_t *parent, int w, int h);

void app_chat_configure(const config_t *cfg, void (*on_select)(int contact_idx))
{
    g_cfg = cfg;
    g_on_select = on_select;
}

lv_obj_t *app_chat_sidebar_panel(void) { return g_sidebar_panel; }
lv_obj_t *app_chat_content_panel(void) { return g_content_panel; }

static void unread_changed(void)
{
    shell_refresh_tile("chat");
}

/* "No contacts yet": the config has no contacts. Built like music's "No
 * music yet": the chat bubble on a round blue sticker, a line for a
 * grown-up. It covers the (empty) shelf and conversation. */
static void nocontacts_create(lv_obj_t *root)
{
    lv_obj_add_flag(g_sidebar_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_content_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(root, 0, 0);

    lv_obj_t *n = tile_obj(root);
    g_nocontacts = n;
    lv_obj_set_flex_grow(n, 1);
    lv_obj_set_height(n, lv_pct(100));

    lv_obj_t *badge = tile_obj(n);
    lv_obj_set_size(badge, 200, 200);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, -110);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge, UI_C_CHAT, 0);
    ui_sticker(badge, 4, 10);
    lv_obj_t *bubble = bubble_build(badge, 120, 85);
    lv_obj_align(bubble, LV_ALIGN_CENTER, 0, 8);   /* optical centre: the tail */

    lv_obj_t *msg = ui_label(n, &font_title_48, UI_C_INK);
    lv_label_set_text(msg, "No contacts yet");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *sub = ui_label(n, &font_chat_28, UI_C_INK_2);
    lv_label_set_text(sub, "A grown-up can add contacts in the settings.");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 140);
}

static void chat_create(lv_obj_t *root)
{
    g_root = root;
    lv_obj_set_style_bg_color(root, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    /* The contacts sit on a white shelf with the page showing around it:
     * that edge is the only divider between them and the conversation. */
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_left(root, SHELF_GAP, 0);
    lv_obj_set_style_pad_ver(root, SHELF_GAP, 0);
    lv_obj_set_style_pad_gap(root, SHELF_GAP, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* Left: sidebar. ui_sidebar_create makes chat's group the default. */
    g_sidebar_panel = lv_obj_create(root);
    lv_obj_set_style_pad_all(g_sidebar_panel, 0, 0);
    lv_obj_set_style_border_width(g_sidebar_panel, 0, 0);
    ui_sidebar_create(g_sidebar_panel, g_cfg, g_on_select);
    ui_sidebar_set_unread_listener(unread_changed);
    g_group = lv_group_get_default();

    /* Right: content panel (the chat panels live here). */
    g_content_panel = lv_obj_create(root);
    lv_obj_set_flex_grow(g_content_panel, 1);
    lv_obj_set_height(g_content_panel, lv_pct(100));
    lv_obj_set_style_bg_opa(g_content_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_content_panel, 0, 0);
    lv_obj_set_style_radius(g_content_panel, 0, 0);
    lv_obj_set_style_pad_all(g_content_panel, 0, 0);
    lv_obj_clear_flag(g_content_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Music peeks in at the right of the header (the song that plays). */
    g_peek = ui_peek_create(root);
    lv_obj_add_flag(g_peek, LV_OBJ_FLAG_FLOATING);
    /* Aligned in the root's content box, which starts SHELF_GAP down, like
     * the chat panel's header: centred on that header. */
    lv_obj_align(g_peek, LV_ALIGN_TOP_RIGHT, -20, (UI_HEAD_H - 48) / 2);
    shell_attach_peek(g_peek, "music");

    if (g_cfg->contact_count == 0) nocontacts_create(root);
}

void app_chat_open_first(void)
{
    if (g_cfg && g_cfg->contact_count > 0) ui_sidebar_open(0);   /* as Enter on the row */
}

static lv_group_t *chat_group(void)
{
    return g_group;
}

/* Nothing to do on show/hide: the root is only hidden, so the selected
 * contact, typed text, scroll position and the group's focus all stay. */
static void chat_on_show(void) {}
static void chat_on_hide(void) {}

static bool chat_on_key(uint32_t key)
{
    if (key == LV_KEY_ESC) return ui_chat_close_emoji_panel();
    return false;
}

/* ---- Live tile ----------------------------------------------------------- */

static struct {
    lv_obj_t   *obj;         /* our container in the switcher's face */
    lv_obj_t   *unread;      /* avatar + count + name */
    lv_obj_t   *avatar;      /* the sender's avatar image... */
    lv_obj_t   *initial;     /* ...or their initial when there is none */
    lv_obj_t   *count;
    lv_obj_t   *name;
    lv_obj_t   *bubble;      /* nobody unread */
    bool        drawn;
    int         shown_count, shown_newest;
    const void *shown_src;
} g_tile;

static lv_obj_t *tile_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *tile_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TILE_INK, 0);
    return l;
}

static void tile_delete_cb(lv_event_t *e)
{
    (void)e;
    memset(&g_tile, 0, sizeof(g_tile));
}

/* A white speech bubble with three dots: "messages", without words.
 * w × h is the body; everything else scales with w (the tile's is 220). */
static lv_obj_t *bubble_build(lv_obj_t *parent, int w, int h)
{
#define BS(v) ((v) * w / TILE_BUBBLE_W)
    lv_obj_t *b = tile_obj(parent);
    lv_obj_set_size(b, w, h + BS(40));
    lv_obj_center(b);

    /* The tail first, so the body covers its upper half. */
    lv_obj_t *tail = tile_obj(b);
    lv_obj_set_size(tail, BS(52), BS(52));
    lv_obj_set_pos(tail, BS(40), h - BS(40));
    lv_obj_set_style_bg_color(tail, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(tail, 450, 0);
    lv_obj_set_style_transform_pivot_x(tail, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(tail, lv_pct(50), 0);

    lv_obj_t *body = tile_obj(b);
    lv_obj_set_size(body, w, h);
    lv_obj_set_style_radius(body, BS(48), 0);
    lv_obj_set_style_bg_color(body, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = tile_obj(body);
        lv_obj_set_size(d, BS(28), BS(28));
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, UI_C_CHAT, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_CENTER, (i - 1) * BS(50), 0);
    }
    return b;
#undef BS
}

static void tile_build_bubble(lv_obj_t *parent)
{
    g_tile.bubble = bubble_build(parent, TILE_BUBBLE_W, TILE_BUBBLE_H);
}

static void tile_build(lv_obj_t *face)
{
    memset(&g_tile, 0, sizeof(g_tile));
    lv_obj_t *o = tile_obj(face);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(o, tile_delete_cb, LV_EVENT_DELETE, NULL);
    g_tile.obj = o;

    lv_obj_t *u = tile_obj(o);
    lv_obj_set_size(u, lv_pct(100), lv_pct(100));
    g_tile.unread = u;

    /* The sidebar's avatar image, at the PNG's own size (no scaling), in a
     * circle: a round container clips it (an image's clip_corner doesn't
     * clip its own pixels). */
    lv_obj_t *ring = tile_obj(u);
    lv_obj_set_size(ring, TILE_AVATAR, TILE_AVATAR);
    lv_obj_set_pos(ring, TILE_INSET, TILE_INSET);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(ring, true, 0);
    lv_obj_set_style_bg_color(ring, UI_C_CHAT_SOFT, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    ui_sticker(ring, 4, 0);
    lv_obj_t *av = lv_image_create(ring);
    lv_obj_set_size(av, TILE_AVATAR, TILE_AVATAR);
    lv_obj_set_pos(av, 0, 0);
    lv_image_set_inner_align(av, LV_IMAGE_ALIGN_CENTER);
    g_tile.avatar = av;
    g_tile.initial = tile_label(ring, &font_title_48);
    lv_obj_set_style_text_color(g_tile.initial, UI_C_CHAT_INK, 0);
    lv_obj_center(g_tile.initial);

    /* The number, big, next to the face. */
    lv_obj_t *c = tile_label(u, &font_count_96);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, TILE_INSET + TILE_AVATAR + 36,
                 TILE_INSET + (TILE_AVATAR - lv_font_get_line_height(&font_count_96)) / 2);
    g_tile.count = c;

    lv_obj_t *n = tile_label(u, &font_title_36);
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(n, TILE_INSET, TILE_INSET + TILE_AVATAR + 20);
    g_tile.name = n;

    tile_build_bubble(o);
}

static int chat_badge(void)
{
    return ui_sidebar_unread_count();
}

/* The first UTF-8 character of `name` (for a contact without an avatar). */
static void first_char(const char *name, char out[8])
{
    size_t n = 1;
    unsigned char c = (unsigned char)name[0];
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    size_t i = 0;
    for (; i < n && name[i]; i++) out[i] = name[i];
    out[i] = 0;
}

static void chat_tile_render(lv_obj_t *face)
{
    if (!g_tile.obj || lv_obj_get_parent(g_tile.obj) != face) {
        if (g_tile.obj) lv_obj_delete(g_tile.obj);
        tile_build(face);
    }

    int count  = chat_badge();
    int newest = count > 0 ? ui_sidebar_newest_unread() : -1;
    const void *src = ui_sidebar_avatar_src(newest);
    /* Nothing changed (several events in one sync each notify): no redraw. */
    if (g_tile.drawn && count == g_tile.shown_count && newest == g_tile.shown_newest &&
        src == g_tile.shown_src)
        return;
    g_tile.drawn = true;
    g_tile.shown_count = count;
    g_tile.shown_newest = newest;
    g_tile.shown_src = src;

    if (count == 0) {
        lv_obj_add_flag(g_tile.unread, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_tile.bubble, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(g_tile.bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_tile.unread, LV_OBJ_FLAG_HIDDEN);

    const char *name = ui_sidebar_contact_name(newest);
    if (src) {
        lv_image_set_src(g_tile.avatar, src);
        lv_obj_add_flag(g_tile.initial, LV_OBJ_FLAG_HIDDEN);
    } else {
        char ini[8];
        first_char(name, ini);
        lv_image_set_src(g_tile.avatar, NULL);
        lv_label_set_text(g_tile.initial, ini);
        lv_obj_clear_flag(g_tile.initial, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_fmt(g_tile.count, "%d", count);
    lv_label_set_text(g_tile.name, name);
}

/* ---- Peek: our news in music's header ------------------------------------ */

static struct {
    lv_obj_t   *ring;
    lv_obj_t   *avatar;
    lv_obj_t   *initial;
    lv_obj_t   *text;
} g_peek_ui;

static void peek_delete_cb(lv_event_t *e)
{
    (void)e;
    memset(&g_peek_ui, 0, sizeof(g_peek_ui));
}

/* The newest unread contact's face and name ("2 new" when more than one
 * contact wrote). Nothing unread: nothing to say. */
static bool chat_peek_render(lv_obj_t *pill)
{
    int count = chat_badge();
    if (count == 0) return false;
    if (!g_peek_ui.ring || lv_obj_get_parent(g_peek_ui.ring) != pill) {
        lv_obj_clean(pill);
        lv_obj_t *ring = tile_obj(pill);
        lv_obj_set_size(ring, PEEK_AVATAR, PEEK_AVATAR);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(ring, true, 0);
        lv_obj_set_style_bg_color(ring, UI_C_CHAT_SOFT, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(ring, peek_delete_cb, LV_EVENT_DELETE, NULL);
        g_peek_ui.ring = ring;
        g_peek_ui.avatar = lv_image_create(ring);
        lv_obj_set_size(g_peek_ui.avatar, PEEK_AVATAR, PEEK_AVATAR);
        lv_image_set_inner_align(g_peek_ui.avatar, LV_IMAGE_ALIGN_STRETCH);
        g_peek_ui.initial = ui_label(ring, &font_title_28, UI_C_CHAT_INK);
        lv_obj_center(g_peek_ui.initial);
        g_peek_ui.text = ui_label(pill, &font_title_28, UI_C_CHAT_INK);
        lv_label_set_long_mode(g_peek_ui.text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(g_peek_ui.text, 320, 0);
    }
    int newest = ui_sidebar_newest_unread();
    const void *src = ui_sidebar_avatar_src(newest);
    const char *name = ui_sidebar_contact_name(newest);
    lv_image_set_src(g_peek_ui.avatar, src);
    if (src) {
        lv_label_set_text(g_peek_ui.initial, "");
    } else {
        char ini[8];
        first_char(name, ini);
        lv_label_set_text(g_peek_ui.initial, ini);
    }
    if (count > 1) lv_label_set_text_fmt(g_peek_ui.text, "%d new", count);
    else           lv_label_set_text(g_peek_ui.text, name);
    return true;
}

/* ---- Key hints (CHATBOX_KEY_HINTS) --------------------------------------- */

static const ui_hint_t k_hints_nocontacts[] = {
    { { "Esc" }, "home", true },
};
static const ui_hint_t k_hints_chat[] = {
    { { "Enter" }, "send", false },
    { { "Win" }, "emoji", false },
    { { "Tab" }, "next contact", false },
    { { "Esc" }, "home", true },
};
static const ui_hint_t k_hints_emoji[] = {
    { { UI_KEY_LEFT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_RIGHT }, "choose", false },
    { { "Enter" }, "add", false },
    { { "PgUp", "PgDn" }, "more", false },
    { { "Esc" }, "close", true },
};
#define N_HINTS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const ui_hint_t *chat_hints(int *n)
{
    if (g_nocontacts)           { *n = N_HINTS(k_hints_nocontacts); return k_hints_nocontacts; }
    if (ui_chat_emoji_open())  { *n = N_HINTS(k_hints_emoji); return k_hints_emoji; }
    *n = N_HINTS(k_hints_chat);   /* a chat is always open */
    return k_hints_chat;
}

const app_t app_chat = {
    .id          = "chat",
    .label       = "Chat",
    .look        = UI_APP_CHAT,
    .create      = chat_create,
    .group       = chat_group,
    .on_show     = chat_on_show,
    .on_hide     = chat_on_hide,
    .on_key      = chat_on_key,
    .tile_render = chat_tile_render,
    .badge       = chat_badge,
    .peek_render = chat_peek_render,
    .hints       = chat_hints,
};

#ifdef CHATBOX_TESTING
bool app_chat_test_no_contacts(void)
{
    return g_nocontacts && !lv_obj_has_flag(g_nocontacts, LV_OBJ_FLAG_HIDDEN);
}

void app_chat_test_tile(app_chat_tile_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->newest = -1;
    if (!g_tile.obj) return;
    out->built  = true;
    out->bubble = !lv_obj_has_flag(g_tile.bubble, LV_OBJ_FLAG_HIDDEN);
    out->unread = !lv_obj_has_flag(g_tile.unread, LV_OBJ_FLAG_HIDDEN);
    out->newest = g_tile.shown_newest;
    out->avatar_src = lv_image_get_src(g_tile.avatar);
    out->initial = !lv_obj_has_flag(g_tile.initial, LV_OBJ_FLAG_HIDDEN);
    snprintf(out->count, sizeof(out->count), "%s", lv_label_get_text(g_tile.count));
    snprintf(out->name, sizeof(out->name), "%s", lv_label_get_text(g_tile.name));
}
#endif
