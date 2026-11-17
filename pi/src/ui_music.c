/**
 * ui_music.c — Music app UI (docs/player-spec.md §7, §8.2–§8.4, D4; the
 * sticker kit, ui_theme.h and docs/design.md).
 *
 * Layout (1280×UI_APP_H)
 *   BROWSE       a 68 px header ("Albums", and chat's peek pill on the
 *                right) over one of two pages, above the 104 px mini
 *                now-playing strip:
 *                  grid        every album as a big cover card, four across
 *                              (g_focus == MUSIC_COL_ALBUMS);
 *                  album page  the selected album's cover, title and
 *                              artist on the left, its songs on the right
 *                              (g_focus == MUSIC_COL_SONGS).
 *                The focused card or song has the focus look (ui_theme.h).
 *   NOW_PLAYING  full screen: 512 px cover, state, title, artist, album,
 *                "Song N of M", progress bar with times, and the three
 *                transport stickers (previous, play/pause, next).
 *   NO_MUSIC     "No music yet" with a big orange note sticker.
 *   volume overlay (1.5 s after PgUp/PgDn) above whichever view is up.
 *
 * Rendering cost (software rendering of a 1280×800 framebuffer on the Pi)
 *   - Cards and rows are virtualised: the grid has a fixed pool of card
 *     widgets (the rows that fit, plus one), the album page a pool of song
 *     rows, re-bound to items as they scroll, so a 500-album library still
 *     has 12 cards. Only bound cards and rows show images.
 *   - Art is the indexer's pre-scaled PNG thumbnails: 256 px on the cards
 *     and the album page, 96 px in the strip (songs show the album's 96 px
 *     thumb at 64), 512 px on Now Playing (§2). A thumbnail is decoded once
 *     and then served from LVGL's image cache (LV_CACHE_DEF_SIZE, sized
 *     for a screen of 256 px cards, see lv_conf.h); a card re-bound to the
 *     same art id does nothing. The 512 px cover is only decoded when Now
 *     Playing is actually shown.
 *   - Every art square is a box with the fallback cover (§7.6: a diagonal
 *     gradient seeded from the title, plus its first letter). Before an
 *     image is used it is opened once through the decoder (which fills the
 *     cache for the draw that follows); a missing or undecodable file keeps
 *     the fallback, so there is never an empty square.
 *   - No slide or scroll animations: a slide redraws the whole screen every
 *     frame. The only animations are small: the equaliser glyphs (one timer
 *     steps their bars, ~8 fps, only while playing and only the visible
 *     ones) and the edge bounces (a 14 px translate of one list for 200 ms).
 *   - Views that are covered are hidden (Now Playing hides the browse page
 *     and the strip), so nothing under an opaque view is invalidated.
 *
 * Live tile (T9, §6): ui_music_tile_render() fills the switcher's Music
 * tile: the 256 px album thumbnail (or the fallback cover), a play/pause
 * glyph and the track title; a big music note when nothing is loaded. One
 * static layout, no flip between an art face and an info face: a flip
 * needs a timer and redraws the whole 520 px tile each time, and half the
 * time it would hide the art — the thing a child who can't read yet
 * recognises the tile by. The tile listener is told of every track/state
 * change; the shell ignores it while the switcher is closed. The same
 * listener refreshes music's peek pill in chat's header
 * (ui_music_peek_render: the song that plays).
 *
 * State
 *   Lists come from library.h into static fixed-capacity structs. The
 *   Albums list is every album in the library (library_load_all_albums,
 *   §7.2), the Songs list the selected album's tracks (§7.1). Selection
 *   memory (§7.4) is kept by id: one album selection and a song selection
 *   per album, each {id, index} so a reload re-finds the id and otherwise
 *   keeps the index (clamped).
 *
 *   "Current track" facts (its album and artist ids, titles, art) come from
 *   library_track_get() on the player's current id, refreshed on every
 *   track_changed callback and on every library reload.
 *
 *   Enter on an album card opens its page; Enter on a song plays it and
 *   opens Now Playing at once (§7.3), so ←/→ skip tracks right away. A song
 *   plays from 0:00 (even the current song); on Now Playing Enter restarts
 *   the current song. Only Space pauses. On Now Playing ↑/↓ are volume and
 *   Esc goes back to the album page; on the album page Esc (or ←) goes back
 *   to the grid.
 */

#include "ui_music.h"
#include "ui_theme.h"
#include "text_nfc.h"

#include "src/misc/cache/lv_cache.h"
#include "src/draw/lv_image_decoder_private.h"   /* lv_image_decoder_dsc_t */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- Look (ui_theme.h) ----------------------------------------------------- */

#define C_INK        UI_C_INK
#define C_INK_2      UI_C_INK_2
#define C_HOT        UI_C_MUSIC         /* music's colour: eq, progress, Play */
#define C_HOT_INK    UI_C_MUSIC_INK
#define C_WHITE      lv_color_hex(0xFFFFFF)

#define SCR_W        UI_W
#define SCR_H        UI_APP_H
#define STRIP_H       104
#define BROWSE_H     (SCR_H - STRIP_H)
#define HEAD_H       UI_HEAD_H
#define VIEW_H       (BROWSE_H - HEAD_H)     /* the grid / the songs list */
#define STRIP_LINE_H    6

/* Grid: four cards across, centred. */
#define GRID_COLS       4
#define CARD_W        280
#define CARD_GAP_X     26
#define CARD_GAP_Y     22
#define CARD_PAD       12
#define CARD_ART      256
#define GRID_X        ((SCR_W - GRID_COLS * CARD_W - (GRID_COLS - 1) * CARD_GAP_X) / 2)
#define GRID_PAD_Y     14
#define CARD_POOL      16

/* Album page: the album on the left, its songs on the right. */
#define PAGE_ART      256
#define ABOUT_X        40
#define ABOUT_W       300
#define SONGS_X       (ABOUT_X + ABOUT_W + 30)
#define SONGS_W       (SCR_W - SONGS_X - 24)
#define LIST_PAD       12                 /* room for the focus ring */
#define SONG_ROW_H     88

#define ART_LIST       96     /* the strip's thumbnail */
#define ART_SONG       64     /* the album thumb, scaled, on song rows */
#define ART_NP        512
#define ART_NP_SMALL   56     /* artist / album icons on Now Playing */

#define POOL_MAX        9

/* ---- Timing (ms) --------------------------------------------------------- */

#define VOL_SHOW_MS   1500
#define EQ_STEP_MS     120

#define VOL_SEGS        20

/* ---- Widgets ------------------------------------------------------------- */

/* An art square: the fallback cover (gradient box + letter) with the image
 * on top when one loads. Rounded; the image is clipped to the corners. */
typedef struct {
    lv_obj_t     *box;
    lv_obj_t     *letter;
    lv_obj_t     *img;
    lv_grad_dsc_t grad;
    int           size;       /* on screen */
    int           thumb;      /* thumbnail size used: 96, 256 or 512 */
    int64_t       art_id;     /* what is shown now */
    uint32_t      hash;
    bool          bound;
    bool          fallback;   /* the fallback is what's visible */
} art_t;

/* The equaliser glyph: three bars. */
typedef struct {
    lv_obj_t *obj;
    lv_obj_t *bar[3];
    bool      shown;
} eq_t;

/* A song row on the album page. */
typedef struct {
    lv_obj_t *obj;
    art_t     art;
    lv_obj_t *no;           /* track number */
    lv_obj_t *title;
    eq_t      eq;
    int       item;         /* bound item, -1 = none */
    unsigned  gen;          /* g_list_gen[col] it was bound with */
    int       text_x, text_w;
} row_t;

/* An album card in the grid. */
typedef struct {
    lv_obj_t *obj;
    art_t     art;
    lv_obj_t *title;
    eq_t      eq;
    int       item;
    unsigned  gen;
} card_t;

static lv_obj_t *g_root;
static lv_obj_t *g_cols;            /* the browse view: header + grid / album page */
static lv_obj_t *g_head_lbl;

static struct {
    lv_obj_t *view;
    lv_obj_t *list;                 /* holds the cards; translated by the bounce */
    card_t    cards[CARD_POOL];
    int       pool;
    int       card_h;
    int       scroll_y;
} g_grid;

static struct {
    lv_obj_t *obj;
    art_t     art;
    lv_obj_t *title, *artist, *sub;
    lv_obj_t *view;
    lv_obj_t *list;                 /* holds the rows; translated by the bounce */
    row_t     rows[POOL_MAX];
    int       pool;
    int       scroll_y;
} g_page;

static lv_obj_t *g_peek;            /* chat's news, in our header */

static struct {
    lv_obj_t *obj;
    lv_obj_t *line;         /* progress (lv_bar) */
    art_t     art;
    lv_obj_t *idle_icon;
    lv_obj_t *idle_t1, *idle_t2;
    lv_obj_t *live;         /* container of the live widgets */
    eq_t      eq;
    lv_obj_t *title, *sub, *time;
    lv_obj_t *state, *state_sym;
} g_strip;

static struct {
    lv_obj_t *obj;
    lv_obj_t *main;
    art_t     art;
    lv_obj_t *badge;
    eq_t      eq;
    lv_obj_t *state_lbl;
    lv_obj_t *title;
    art_t     artist_art;
    lv_obj_t *artist;
    art_t     album_art;
    lv_obj_t *album;
    lv_obj_t *count;
    lv_obj_t *bar;
    lv_obj_t *elapsed, *remaining;
    lv_obj_t *pp_sym;       /* play/pause glyph on the Play sticker */
    bool      art_pending;  /* the 512 cover waits until NP is shown */
} g_np;

static struct {
    lv_obj_t *obj;
    lv_obj_t *value, *msg;
    lv_obj_t *seg[VOL_SEGS];
    lv_obj_t *cap;
} g_vol;

static lv_obj_t *g_nomusic;

/* The switcher's Music tile (lives in the switcher's tile, not in g_root). */
#define TILE_INSET      6     /* the cover's ring is drawn outside its box */
#define TILE_ART      256
#define TILE_GLYPH     96
static struct {
    lv_obj_t *obj;          /* our container in the tile's face */
    lv_obj_t *live;
    art_t     art;
    lv_obj_t *glyph_bg, *glyph;
    lv_obj_t *title;
    lv_obj_t *idle;         /* nothing loaded: the note */
    int       w;            /* the face's width */
} g_tile;
static void (*g_tile_listener)(void);

/* Our peek pill in chat's header (ui_music_peek_render). */
static struct {
    lv_obj_t *eq;
    lv_obj_t *text;
} g_peek_ui;

static lv_timer_t *g_eq_timer;
static lv_timer_t *g_vol_timer;
static int         g_eq_phase;
static bool        g_eq_running;

/* ---- State --------------------------------------------------------------- */

static library_t                *g_lib;
static const music_player_ops_t *g_ops;

static library_all_albums_t g_albums;
static library_tracks_t     g_tracks;
static int64_t              g_tracks_of = -1;  /* album id g_tracks belongs to */
static bool                 g_force_load;
static unsigned             g_list_gen[MUSIC_COL_COUNT];  /* bumped on every (re)load */

static music_view_t g_view = MUSIC_VIEW_NO_MUSIC;
static int          g_focus;                    /* ALBUMS: the grid; SONGS: the album page */
static int          g_sel[MUSIC_COL_COUNT] = { -1, -1 };
static int64_t      g_album_sel_id;             /* remembered album, 0 = none */
static bool         g_visible;                  /* music app on screen */

/* Selection memory (§7.4): parent id → {selected child id, index}. */
typedef struct {
    int64_t key;
    int64_t sel_id;
    int     sel_idx;
    bool    used;
} mem_t;

#define ALBUM_MEM   1024
static mem_t g_album_mem[ALBUM_MEM];     /* album → song */
static int   g_album_mem_next;

/* The current track, as the library knows it. */
static struct {
    bool    valid;          /* a track is current                 */
    bool    known;          /* ...and found in the library        */
    int64_t id, album_id, art_id, artist_art_id;
    int64_t duration_ms;
    char    title[LIBRARY_NAME_LEN];
    char    album[LIBRARY_NAME_LEN];
    char    artist[LIBRARY_NAME_LEN];
} g_now;

static player_state_t g_pstate = PLAYER_IDLE;
static int            g_last_sec = -1, g_last_total = -2, g_last_prog = -1;

/* Queue building: paths are packed into one pool (player.h copies them). */
static library_track_info_t    g_info;
static player_track_t          g_q[PLAYER_MAX_QUEUE];
static char                    g_qpaths[PLAYER_PATH_POOL];

#ifdef CHATBOX_TESTING
static int g_bounces, g_last_bounce;
#endif

static void render_all(void);
static void update_now_widgets(void);
static void update_state_widgets(void);
static void update_position(bool force);
static void eq_timer_update(void);
static void set_view(music_view_t v);

/* ---- Small helpers ------------------------------------------------------- */

static void fmt_time(char *out, size_t n, int64_t ms)
{
    if (ms < 0) ms = 0;
    int64_t s = ms / 1000;
    snprintf(out, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;                  /* FNV-1a */
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

/* Decode one UTF-8 code point at *p (advancing it); 0 at the end. */
static uint32_t utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    int n = 0;
    if (c == 0) return 0;
    if (c < 0x80)                { n = 0; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { (*p)++; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p += n + 1;
    return c;
}

static void utf8_put(char *out, uint32_t c)
{
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; }
    else if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); out[2] = 0;
    } else if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F)); out[3] = 0;
    } else {
        out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
        out[4] = 0;
    }
}

/* The title's first letter, upper-cased (ASCII, Latin-1, Greek), skipping
 * leading spaces and punctuation. "" when there is none the font has. */
static void first_letter(const char *title, const lv_font_t *font, char out[8])
{
    out[0] = 0;
    const char *p = title ? title : "";
    uint32_t c;
    while ((c = utf8_next(&p)) != 0) {
        if (c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z')))
            continue;                           /* space, punctuation */
        break;
    }
    if (c == 0) return;
    if (c >= 'a' && c <= 'z') c -= 32;
    else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 32;
    else if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) c -= 32;
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, c, 0) || g.box_w == 0) return;
    utf8_put(out, c);
}

/* Wrap `text` in l (width w) to at most `lines` lines, "..." when cut.
 * Returns the label's height. */
static int label_set_lines(lv_obj_t *l, const char *text, int w, int lines)
{
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    int lh = lv_font_get_line_height(f);
    lv_point_t sz;
    lv_text_get_size(&sz, text, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    int h = sz.y > lh * lines ? lh * lines : (sz.y < lh ? lh : sz.y);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, h);
    lv_label_set_long_mode(l, sz.y > lh * lines ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
    return h;
}

/* ---- Art (§7.6) ---------------------------------------------------------- */

static void art_create(art_t *a, lv_obj_t *parent, int size, int thumb, int radius)
{
    memset(a, 0, sizeof(*a));
    a->size  = size;
    a->thumb = thumb;
    a->box = ui_obj(parent);
    lv_obj_set_size(a->box, size, size);
    lv_obj_set_style_bg_opa(a->box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->box, radius, 0);
    lv_obj_set_style_clip_corner(a->box, true, 0);

    const lv_font_t *f = size >= 96 ? &font_title_48 : &font_title_36;
    a->letter = ui_label(a->box, f, C_WHITE);
    lv_obj_center(a->letter);
    if (size > 96) {
        /* The Now Playing cover: the 48 px letter scaled up. Transformed
         * only when this area is redrawn (a track change or pause). */
        int scale = (size * 256 * 2) / (5 * 48);    /* letter ≈ 40 % of the cover */
        lv_obj_set_style_transform_scale(a->letter, scale, 0);
        lv_obj_set_style_transform_pivot_x(a->letter, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(a->letter, lv_pct(50), 0);
    }

    a->img = lv_image_create(a->box);
    lv_obj_set_size(a->img, size, size);
    lv_obj_center(a->img);
    if (size != thumb) lv_image_set_inner_align(a->img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);
}

/* True when `src` ("A:<path>") opens through the image decoder: the file
 * exists and decodes. The decoded image goes into LVGL's image cache, so
 * the draw that follows doesn't decode it again. */
static bool art_image_ok(const char *src)
{
    if (access(src + 2, R_OK) != 0) return false;
    lv_image_decoder_dsc_t d;
    if (lv_image_decoder_open(&d, src, NULL) != LV_RESULT_OK) return false;
    bool ok = d.decoded != NULL || d.header.w > 0;
    lv_image_decoder_close(&d);
    return ok;
}

static void art_show_fallback(art_t *a, const char *title)
{
    uint32_t h = hash_str(title);
    uint16_t h1 = (uint16_t)(h % 360);
    uint16_t h2 = (uint16_t)((h1 + 35 + (h >> 16) % 70) % 360);
    lv_color_t colors[2] = {
        lv_color_hsv_to_rgb(h1, 70, 92),
        lv_color_hsv_to_rgb(h2, 85, 58),
    };
    lv_gradient_init_stops(&a->grad, colors, NULL, NULL, 2);
    lv_grad_linear_init(&a->grad, 0, 0, lv_pct(100), lv_pct(100), LV_GRAD_EXTEND_PAD);
    lv_obj_set_style_bg_grad(a->box, &a->grad, 0);
    lv_obj_set_style_bg_opa(a->box, LV_OPA_COVER, 0);

    char letter[8];
    first_letter(title, lv_obj_get_style_text_font(a->letter, 0), letter);
    if (letter[0]) {
        lv_obj_set_style_text_font(a->letter, a->size >= 96 ? &font_title_48 : &font_title_36, 0);
        lv_label_set_text(a->letter, letter);
    } else {
        lv_obj_set_style_text_font(a->letter, &lv_font_montserrat_36, 0);
        lv_label_set_text(a->letter, LV_SYMBOL_AUDIO);
    }
    ui_set_hidden(a->letter, false);
    ui_set_hidden(a->img, true);
    lv_obj_invalidate(a->box);
    a->fallback = true;
}

/* Show art `art_id` (0 = none) for an item called `title`. Cheap when it
 * is already showing. */
static void art_set(art_t *a, int64_t art_id, const char *title)
{
    uint32_t h = hash_str(title);
    if (a->bound && a->art_id == art_id && a->hash == h) return;
    a->bound  = true;
    a->art_id = art_id;
    a->hash   = h;

    char path[PATH_MAX];
    char src[PATH_MAX + 2];
    if (art_id > 0 && library_art_path(g_lib, art_id, a->thumb, path, sizeof(path))) {
        snprintf(src, sizeof(src), "A:%s", path);
        if (art_image_ok(src)) {
            lv_image_set_src(a->img, src);
            ui_set_hidden(a->img, false);
            ui_set_hidden(a->letter, true);
            /* The image covers the box: don't draw the gradient under it. */
            lv_obj_set_style_bg_opa(a->box, LV_OPA_TRANSP, 0);
            a->fallback = false;
            return;
        }
    }
    art_show_fallback(a, title);
}

static void art_unbind(art_t *a)
{
    a->bound = false;
}

/* ---- Equaliser glyph ----------------------------------------------------- */

#define EQ_W   28
#define EQ_H   26

static void eq_create(eq_t *e, lv_obj_t *parent)
{
    e->obj = ui_obj(parent);
    lv_obj_set_size(e->obj, EQ_W, EQ_H);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = ui_obj(e->obj);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, C_HOT, 0);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_set_size(b, 7, EQ_H * 45 / 100);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 10 + (i == 2 ? 1 : 0), 0);
        e->bar[i] = b;
    }
    lv_obj_add_flag(e->obj, LV_OBJ_FLAG_HIDDEN);
    e->shown = false;
}

static void eq_show(eq_t *e, bool show)
{
    if (e->shown == show) return;
    e->shown = show;
    ui_set_hidden(e->obj, !show);
}

/* Bar heights in % for each animation step (three bars out of phase). */
static const uint8_t k_eq_levels[] = { 30, 55, 85, 100, 70, 45, 25, 60, 90, 50 };
#define EQ_LEVELS ((int)(sizeof(k_eq_levels) / sizeof(k_eq_levels[0])))

static void eq_apply(eq_t *e, bool animate)
{
    if (!e->shown) return;
    for (int i = 0; i < 3; i++) {
        int pct = animate ? k_eq_levels[(g_eq_phase * (i + 1) + i * 3) % EQ_LEVELS] : 45;
        int h = EQ_H * pct / 100;
        if (lv_obj_get_height(e->bar[i]) != h) {
            lv_obj_set_height(e->bar[i], h);
            lv_obj_align(e->bar[i], LV_ALIGN_BOTTOM_LEFT, i * 10 + (i == 2 ? 1 : 0), 0);
        }
    }
}

static bool eq_should_animate(void)
{
    return g_visible && g_pstate == PLAYER_PLAYING && g_view != MUSIC_VIEW_NO_MUSIC;
}

static void eq_apply_all(bool animate)
{
    if (g_view == MUSIC_VIEW_NOW_PLAYING) {
        eq_apply(&g_np.eq, animate);
        return;
    }
    if (g_view != MUSIC_VIEW_BROWSE) return;
    eq_apply(&g_strip.eq, animate);
    for (int j = 0; j < g_grid.pool; j++)
        if (g_grid.cards[j].item >= 0) eq_apply(&g_grid.cards[j].eq, animate);
    for (int j = 0; j < g_page.pool; j++)
        if (g_page.rows[j].item >= 0) eq_apply(&g_page.rows[j].eq, animate);
}

static void eq_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!eq_should_animate()) {
        eq_timer_update();
        return;
    }
    g_eq_phase++;
    eq_apply_all(true);
}

/* Run the eq timer only while something visible animates; otherwise put
 * every glyph at rest. */
static void eq_timer_update(void)
{
    if (!g_eq_timer) return;
    g_eq_running = eq_should_animate();
    if (g_eq_running) {
        lv_timer_resume(g_eq_timer);
    } else {
        lv_timer_pause(g_eq_timer);
        eq_apply_all(false);
    }
}

/* ---- Bounce feedback ----------------------------------------------------- */

_Static_assert(MUSIC_BOUNCE_UP == (int)UI_BOUNCE_UP && MUSIC_BOUNCE_DOWN == (int)UI_BOUNCE_DOWN &&
               MUSIC_BOUNCE_LEFT == (int)UI_BOUNCE_LEFT && MUSIC_BOUNCE_RIGHT == (int)UI_BOUNCE_RIGHT,
               "MUSIC_BOUNCE_* are ui_bounce_t values");

static void bounce(lv_obj_t *obj, int kind)
{
#ifdef CHATBOX_TESTING
    g_bounces++;
    g_last_bounce = kind;
#endif
    ui_bounce(obj, (ui_bounce_t)kind);
}

/* ---- Selection memory (§7.4) --------------------------------------------- */

static mem_t *mem_find(mem_t *tab, int n, int64_t key)
{
    for (int i = 0; i < n; i++)
        if (tab[i].used && tab[i].key == key) return &tab[i];
    return NULL;
}

static void mem_put(mem_t *tab, int n, int *next, int64_t key, int64_t sel_id, int sel_idx)
{
    if (key <= 0) return;
    mem_t *m = mem_find(tab, n, key);
    if (!m) {
        for (int i = 0; i < n && !m; i++)
            if (!tab[i].used) m = &tab[i];
        if (!m) {                       /* full: reuse round-robin */
            m = &tab[*next];
            *next = (*next + 1) % n;
        }
    }
    m->used    = true;
    m->key     = key;
    m->sel_id  = sel_id;
    m->sel_idx = sel_idx;
}

static int64_t sel_album_id(void)
{
    return g_sel[0] >= 0 && g_sel[0] < g_albums.count ? g_albums.items[g_sel[0]].id : 0;
}

static int64_t sel_track_id(void)
{
    return g_sel[1] >= 0 && g_sel[1] < g_tracks.count ? g_tracks.items[g_sel[1]].id : 0;
}

static const library_album_t *sel_album(void)
{
    return g_sel[0] >= 0 && g_sel[0] < g_albums.count ? &g_albums.items[g_sel[0]] : NULL;
}

static void remember_song(void)
{
    mem_put(g_album_mem, ALBUM_MEM, &g_album_mem_next, sel_album_id(),
            sel_track_id(), g_sel[1]);
}

/* Select song `idx` of the loaded album (no memory lookup). */
static void select_song(int idx)
{
    g_sel[1] = g_tracks.count > 0 ? idx : -1;
    remember_song();
}

/* The index of the playing song in the loaded album, or -1. */
static int playing_song_index(void)
{
    if (!g_now.known || g_now.album_id != g_tracks_of) return -1;
    int p = library_tracks_find(&g_tracks, g_now.id, -1);
    return p >= 0 && g_tracks.items[p].id == g_now.id ? p : -1;
}

/* Select album `idx` and load its songs, landing per §7.4: the playing
 * song if it's in this album, else the song last selected there, else the
 * first. */
static void select_album(int idx)
{
    g_sel[0] = g_albums.count > 0 ? idx : -1;
    int64_t alid = sel_album_id();
    g_album_sel_id = alid;
    if (alid != g_tracks_of || g_force_load) {
        if (alid > 0) library_load_tracks(g_lib, alid, &g_tracks);
        else g_tracks.count = g_tracks.total = 0;
        g_tracks_of = alid;
        g_list_gen[MUSIC_COL_SONGS]++;
    }
    int si = -1;
    if (g_tracks.count > 0) {
        si = playing_song_index();
        if (si < 0) {
            mem_t *m = mem_find(g_album_mem, ALBUM_MEM, alid);
            si = m ? library_tracks_find(&g_tracks, m->sel_id, m->sel_idx) : 0;
        }
    }
    select_song(si);
}

/* ---- The current track --------------------------------------------------- */

static void refresh_now(void)
{
    int64_t id = g_ops ? g_ops->current_id() : 0;
    memset(&g_now, 0, sizeof(g_now));
    g_now.duration_ms = -1;
    if (id <= 0) return;
    g_now.valid = true;
    g_now.id = id;
    if (g_lib && library_track_get(g_lib, id, &g_info) == LIBRARY_OK) {
        g_now.known       = true;
        g_now.album_id    = g_info.album_id;
        g_now.art_id      = g_info.art_id;
        g_now.artist_art_id = g_info.artist_art_id;
        g_now.duration_ms = g_info.duration_ms;
        library_utf8_copy(g_now.title, sizeof(g_now.title), g_info.title);
        library_utf8_copy(g_now.album, sizeof(g_now.album), g_info.album_title);
        library_utf8_copy(g_now.artist, sizeof(g_now.artist), g_info.artist_name);
    } else {
        /* Gone from the library (§7.7) but still playing: the file name. */
        const char *p = g_ops->current_path ? g_ops->current_path() : "";
        const char *b = strrchr(p, '/');
        library_utf8_copy(g_now.title, sizeof(g_now.title), b ? b + 1 : p);
        text_utf8_nfc(g_now.title);
    }
}

/* "Various Artists" albums don't name their artist on the row. */
static bool shows_artist(const library_album_t *a)
{
    return a->artist[0] && strcmp(a->artist, "Various Artists") != 0;
}

/* ---- Browse: the album grid and an album's page -------------------------- */

static int songs_list_w(void) { return SONGS_W - 2 * LIST_PAD; }

static void row_create(row_t *r, lv_obj_t *parent)
{
    int w = songs_list_w(), h = SONG_ROW_H;
    memset(r, 0, sizeof(*r));
    r->item = -1;
    r->obj = ui_obj(parent);
    lv_obj_set_size(r->obj, w, h);
    lv_obj_set_style_radius(r->obj, UI_RADIUS, 0);

    r->no = ui_label(r->obj, &font_chat_28, C_INK_2);
    lv_obj_set_width(r->no, 44);
    lv_obj_set_style_text_align(r->no, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(r->no, LV_ALIGN_LEFT_MID, 6, 0);
    art_create(&r->art, r->obj, ART_SONG, ART_LIST, 12);
    lv_obj_align(r->art.box, LV_ALIGN_LEFT_MID, 64, 0);
    r->text_x = 64 + ART_SONG + 18;
    r->text_w = w - r->text_x - 16 - EQ_W - 16;
    r->title = ui_label(r->obj, &font_chat_28, C_INK);
    eq_create(&r->eq, r->obj);
    lv_obj_align(r->eq.obj, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_add_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
}

/* Fill row r with song i of the selected album (texts, art). */
static void row_bind(row_t *r, int i)
{
    const library_track_t *t = &g_tracks.items[i];
    /* Songs show their album's cover (the album is the selected one). */
    const library_album_t *al = sel_album();
    char no[16];
    snprintf(no, sizeof(no), "%d", t->track_no > 0 ? t->track_no : i + 1);
    lv_label_set_text(r->no, no);
    if (r->item != i || r->gen != g_list_gen[MUSIC_COL_SONGS]) {
        int th = label_set_lines(r->title, t->title, r->text_w, 2);
        lv_obj_set_pos(r->title, r->text_x, (SONG_ROW_H - th) / 2);
    }
    /* The art belongs to the album, titled by the album. */
    art_set(&r->art, al ? al->art_id : 0, al ? al->title : t->title);
    r->item = i;
    r->gen  = g_list_gen[MUSIC_COL_SONGS];
}

static void card_create(card_t *c, lv_obj_t *parent)
{
    memset(c, 0, sizeof(*c));
    c->item = -1;
    c->obj = ui_obj(parent);
    lv_obj_set_size(c->obj, CARD_W, g_grid.card_h);
    lv_obj_set_style_radius(c->obj, UI_RADIUS, 0);
    art_create(&c->art, c->obj, CARD_ART, CARD_ART, 18);
    lv_obj_set_pos(c->art.box, CARD_PAD, CARD_PAD);
    c->title = ui_label(c->obj, &font_chat_28, C_INK);
    lv_obj_set_pos(c->title, CARD_PAD, CARD_PAD + CARD_ART + 8);
    eq_create(&c->eq, c->obj);
    lv_obj_set_pos(c->eq.obj, CARD_W - CARD_PAD - EQ_W, CARD_PAD + CARD_ART + 14);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
}

static void card_bind(card_t *c, int i)
{
    const library_album_t *al = &g_albums.items[i];
    if (c->item != i || c->gen != g_list_gen[MUSIC_COL_ALBUMS])
        label_set_lines(c->title, al->title, CARD_ART - EQ_W - 10, 2);
    art_set(&c->art, al->art_id, al->title);
    c->item = i;
    c->gen  = g_list_gen[MUSIC_COL_ALBUMS];
}

static void card_unbind(card_t *c)
{
    ui_set_hidden(c->obj, true);
    eq_show(&c->eq, false);
    if (c->item >= 0) {
        c->item = -1;
        art_unbind(&c->art);
    }
}

/* Scroll so the selected card's row sits mid-view (clamped), then bind the
 * card pool to the visible rows. */
static void render_grid(void)
{
    if (!g_grid.view) return;
    int n = g_albums.count, sel = g_sel[0];
    int pitch = g_grid.card_h + CARD_GAP_Y;
    int rows = (n + GRID_COLS - 1) / GRID_COLS;
    int max = rows * pitch - CARD_GAP_Y + 2 * GRID_PAD_Y - VIEW_H;
    if (max < 0) max = 0;
    int y = sel >= 0 ? (sel / GRID_COLS) * pitch + GRID_PAD_Y - (VIEW_H - g_grid.card_h) / 2 : 0;
    if (y > max) y = max;
    if (y < 0) y = 0;
    g_grid.scroll_y = y;

    int first = (y - GRID_PAD_Y) / pitch;
    if (first < 0) first = 0;
    int64_t playing = g_now.known ? g_now.album_id : 0;
    for (int j = 0; j < g_grid.pool; j++) {
        card_t *c = &g_grid.cards[j];
        int item = first * GRID_COLS + j;
        if (item >= n) {
            /* Hide even an unbound card: a reload unbinds every card but
             * leaves it showing, so when the list shrank the old last item
             * stayed on screen. */
            card_unbind(c);
            continue;
        }
        card_bind(c, item);
        lv_obj_set_pos(c->obj, GRID_X + (item % GRID_COLS) * (CARD_W + CARD_GAP_X),
                       GRID_PAD_Y + (item / GRID_COLS) * pitch - y);
        ui_set_hidden(c->obj, false);
        bool focused = item == sel && g_focus == MUSIC_COL_ALBUMS && g_view == MUSIC_VIEW_BROWSE;
        ui_item_style(c->obj, focused ? UI_ITEM_FOCUSED : UI_ITEM_IDLE, UI_APP_MUSIC);
        eq_show(&c->eq, playing != 0 && g_albums.items[item].id == playing);
    }
}

/* The album page: the selected album on the left, its songs (scrolled so
 * the selection sits mid-list, clamped) on the right. */
static void render_page(void)
{
    if (!g_page.obj) return;
    const library_album_t *al = sel_album();
    if (al) {
        art_set(&g_page.art, al->art_id, al->title);
        label_set_lines(g_page.title, al->title, ABOUT_W, 3);
        lv_label_set_text(g_page.artist, shows_artist(al) ? al->artist : "");
        char sub[64];
        int n = al->track_count;
        if (al->year > 0) snprintf(sub, sizeof(sub), "%d \xC2\xB7 %d song%s", al->year, n, n == 1 ? "" : "s");
        else              snprintf(sub, sizeof(sub), "%d song%s", n, n == 1 ? "" : "s");
        lv_label_set_text(g_page.sub, sub);
    }

    int n = g_tracks.count, h = SONG_ROW_H, sel = g_sel[1];
    int view_h = VIEW_H - 2 * LIST_PAD;
    int max = n * h - view_h;
    if (max < 0) max = 0;
    int y = sel >= 0 ? sel * h - (view_h - h) / 2 : 0;
    if (y > max) y = max;
    if (y < 0) y = 0;
    g_page.scroll_y = y;

    int first = y / h;
    for (int j = 0; j < g_page.pool; j++) {
        row_t *r = &g_page.rows[j];
        int item = first + j;
        if (item >= n) {
            ui_set_hidden(r->obj, true);
            eq_show(&r->eq, false);
            if (r->item >= 0) {
                r->item = -1;
                art_unbind(&r->art);
            }
            continue;
        }
        row_bind(r, item);
        lv_obj_set_y(r->obj, LIST_PAD + item * h - y);
        ui_set_hidden(r->obj, false);
        bool focused = item == sel && g_focus == MUSIC_COL_SONGS && g_view == MUSIC_VIEW_BROWSE;
        ui_item_style(r->obj, focused ? UI_ITEM_FOCUSED : UI_ITEM_IDLE, UI_APP_MUSIC);
        int64_t id = g_tracks.items[item].id;
        eq_show(&r->eq, g_now.known && id == g_now.id);
    }
}

/* Show the grid or the album page (whichever g_focus says) and draw it. */
static void render_all(void)
{
    if (!g_cols) return;
    bool grid = g_focus == MUSIC_COL_ALBUMS;
    ui_set_hidden(g_grid.view, !grid);
    ui_set_hidden(g_page.obj, grid);
    lv_obj_set_style_text_color(g_head_lbl, grid ? C_HOT_INK : C_INK_2, 0);
    if (grid) render_grid();
    else      render_page();
    eq_apply_all(eq_should_animate());
}

static void browse_create(lv_obj_t *parent)
{
    g_cols = ui_obj(parent);
    lv_obj_set_size(g_cols, SCR_W, BROWSE_H);

    g_head_lbl = ui_label(g_cols, &font_title_36, C_HOT_INK);
    lv_label_set_text(g_head_lbl, "Albums");
    lv_obj_align(g_head_lbl, LV_ALIGN_TOP_LEFT, ABOUT_X,
                 (HEAD_H - lv_font_get_line_height(&font_title_36)) / 2);

    /* The grid. */
    int lh = lv_font_get_line_height(&font_chat_28);
    g_grid.card_h = CARD_PAD + CARD_ART + 8 + 2 * lh + CARD_PAD;
    g_grid.view = ui_obj(g_cols);
    lv_obj_set_pos(g_grid.view, 0, HEAD_H);
    lv_obj_set_size(g_grid.view, SCR_W, VIEW_H);
    g_grid.list = ui_obj(g_grid.view);
    lv_obj_set_size(g_grid.list, SCR_W, VIEW_H);
    lv_obj_add_flag(g_grid.list, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int pitch = g_grid.card_h + CARD_GAP_Y;
    g_grid.pool = ((VIEW_H + pitch - 1) / pitch + 1) * GRID_COLS;
    if (g_grid.pool > CARD_POOL) g_grid.pool = CARD_POOL;
    for (int j = 0; j < g_grid.pool; j++) card_create(&g_grid.cards[j], g_grid.list);

    /* The album page. */
    g_page.obj = ui_obj(g_cols);
    lv_obj_set_pos(g_page.obj, 0, HEAD_H);
    lv_obj_set_size(g_page.obj, SCR_W, VIEW_H);
    lv_obj_t *about = ui_obj(g_page.obj);
    lv_obj_set_pos(about, ABOUT_X, 8);
    lv_obj_set_size(about, ABOUT_W, VIEW_H - 8);
    lv_obj_set_flex_flow(about, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(about, 10, 0);
    art_create(&g_page.art, about, PAGE_ART, PAGE_ART, 22);
    g_page.title = ui_label(about, &font_title_36, C_INK);
    lv_obj_set_style_pad_top(g_page.title, 8, 0);
    g_page.artist = ui_label(about, &font_chat_28, C_INK_2);
    ui_label_one_line(g_page.artist, ABOUT_W);
    g_page.sub = ui_label(about, &font_chat_28, C_INK_2);
    ui_label_one_line(g_page.sub, ABOUT_W);

    g_page.view = ui_obj(g_page.obj);
    lv_obj_set_pos(g_page.view, SONGS_X, 0);
    lv_obj_set_size(g_page.view, SONGS_W, VIEW_H);
    g_page.list = ui_obj(g_page.view);
    lv_obj_set_size(g_page.list, SONGS_W, VIEW_H);
    lv_obj_add_flag(g_page.list, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    g_page.pool = (VIEW_H + SONG_ROW_H - 1) / SONG_ROW_H + 1;
    if (g_page.pool > POOL_MAX) g_page.pool = POOL_MAX;
    for (int j = 0; j < g_page.pool; j++) {
        row_create(&g_page.rows[j], g_page.list);
        lv_obj_set_x(g_page.rows[j].obj, LIST_PAD);
    }
    lv_obj_add_flag(g_page.obj, LV_OBJ_FLAG_HIDDEN);

    /* Chat peeks in at the right of the header (a contact wrote). */
    g_peek = ui_peek_create(g_cols);
    lv_obj_align(g_peek, LV_ALIGN_TOP_RIGHT, -40, (HEAD_H - 48) / 2);
}

/* ---- Strip --------------------------------------------------------------- */

#define STRIP_ART   76

static void strip_create(lv_obj_t *parent)
{
    lv_obj_t *s = ui_obj(parent);
    g_strip.obj = s;
    lv_obj_set_pos(s, 0, BROWSE_H);
    lv_obj_set_size(s, SCR_W, STRIP_H);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, UI_C_SURFACE, 0);

    int art_y = STRIP_LINE_H + (STRIP_H - STRIP_LINE_H - STRIP_ART) / 2;

    /* Idle: a soft music-note square and the hint. */
    g_strip.idle_icon = ui_obj(s);
    lv_obj_set_pos(g_strip.idle_icon, 24, art_y);
    lv_obj_set_size(g_strip.idle_icon, STRIP_ART, STRIP_ART);
    lv_obj_set_style_radius(g_strip.idle_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_strip.idle_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_strip.idle_icon, UI_C_MUSIC_SOFT, 0);
    lv_obj_t *note = ui_label(g_strip.idle_icon, &lv_font_montserrat_36, C_HOT_INK);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_center(note);
    g_strip.idle_t1 = ui_label(s, &font_title_28, C_INK);
    lv_label_set_text(g_strip.idle_t1, "Nothing playing yet");
    lv_obj_set_pos(g_strip.idle_t1, 124, 18);
    g_strip.idle_t2 = ui_label(s, &font_chat_28, C_INK_2);
    lv_label_set_text(g_strip.idle_t2, "Pick a song and press Enter");
    lv_obj_set_pos(g_strip.idle_t2, 124, 56);

    /* Live. */
    g_strip.live = ui_obj(s);
    lv_obj_set_size(g_strip.live, SCR_W, STRIP_H);
    g_strip.line = lv_bar_create(g_strip.live);
    lv_obj_remove_style_all(g_strip.line);
    lv_obj_set_size(g_strip.line, SCR_W, STRIP_LINE_H);
    lv_obj_set_pos(g_strip.line, 0, 0);
    lv_obj_set_style_bg_opa(g_strip.line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_strip.line, UI_C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_strip.line, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_strip.line, C_HOT, LV_PART_INDICATOR);
    lv_bar_set_range(g_strip.line, 0, 1000);

    art_create(&g_strip.art, g_strip.live, STRIP_ART, ART_LIST, 14);
    lv_obj_set_pos(g_strip.art.box, 24, art_y);

    eq_create(&g_strip.eq, g_strip.live);
    lv_obj_set_pos(g_strip.eq.obj, 124, 22);
    g_strip.title = ui_label(g_strip.live, &font_title_28, C_INK);
    lv_obj_set_pos(g_strip.title, 124 + EQ_W + 12, 16);
    ui_label_one_line(g_strip.title, 760 - EQ_W - 12);
    g_strip.sub = ui_label(g_strip.live, &font_chat_28, C_INK_2);
    lv_obj_set_pos(g_strip.sub, 124, 56);
    ui_label_one_line(g_strip.sub, 760);

    g_strip.time = ui_label(g_strip.live, &font_chat_28, C_INK_2);
    lv_obj_set_width(g_strip.time, 230);
    lv_obj_set_style_text_align(g_strip.time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_strip.time, LV_ALIGN_RIGHT_MID, -(28 + 64 + 24), STRIP_LINE_H / 2);

    /* What Space would do, on a small orange sticker. */
    g_strip.state = ui_obj(g_strip.live);
    lv_obj_set_size(g_strip.state, 64, 64);
    lv_obj_set_style_radius(g_strip.state, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_strip.state, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_strip.state, C_HOT, 0);
    ui_sticker(g_strip.state, 3, 0);
    lv_obj_align(g_strip.state, LV_ALIGN_RIGHT_MID, -28, STRIP_LINE_H / 2);
    g_strip.state_sym = ui_label(g_strip.state, &lv_font_montserrat_24, UI_C_ON_MUSIC);
    lv_label_set_text(g_strip.state_sym, LV_SYMBOL_PLAY);
    lv_obj_center(g_strip.state_sym);
}

/* ---- Now Playing --------------------------------------------------------- */

#define NP_ART_X    72
#define NP_ART_Y   ((SCR_H - ART_NP) / 2)
#define NP_INFO_X  640
#define NP_INFO_W  580

/* A round transport button: white with an ink ring, or (main) the orange
 * Play sticker. */
static lv_obj_t *np_button(lv_obj_t *parent, int size, bool main, const char *sym,
                           lv_obj_t **sym_out)
{
    lv_obj_t *b = ui_obj(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, main ? C_HOT : UI_C_SURFACE, 0);
    ui_sticker(b, main ? 4 : 3, main ? 6 : 0);
    lv_obj_t *l = ui_label(b, main ? &lv_font_montserrat_48 : &lv_font_montserrat_36,
                           main ? UI_C_ON_MUSIC : C_INK);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    if (sym_out) *sym_out = l;
    return b;
}

static void np_create(lv_obj_t *parent)
{
    lv_obj_t *np = ui_obj(parent);
    g_np.obj = np;
    lv_obj_set_size(np, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(np, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(np, UI_C_BG, 0);

    g_np.main = ui_obj(np);
    lv_obj_set_size(g_np.main, SCR_W, SCR_H);

    art_create(&g_np.art, g_np.main, ART_NP, ART_NP, 30);
    lv_obj_set_pos(g_np.art.box, NP_ART_X, NP_ART_Y);
    /* Paused: a white pause sticker on the cover. */
    g_np.badge = ui_obj(g_np.main);
    lv_obj_set_size(g_np.badge, 150, 150);
    lv_obj_set_pos(g_np.badge, NP_ART_X + (ART_NP - 150) / 2, NP_ART_Y + (ART_NP - 150) / 2);
    lv_obj_set_style_radius(g_np.badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_np.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_np.badge, UI_C_SURFACE, 0);
    ui_sticker(g_np.badge, 4, 8);
    lv_obj_t *ps = ui_label(g_np.badge, &lv_font_montserrat_48, C_INK);
    lv_label_set_text(ps, LV_SYMBOL_PAUSE);
    lv_obj_center(ps);
    lv_obj_add_flag(g_np.badge, LV_OBJ_FLAG_HIDDEN);

    /* Top-aligned with the cover and running to the bottom of the screen:
     * the transport row follows the text, so a two-line title moves it
     * down instead of pushing it out (it carries its own room for the
     * Play sticker's ring and solid shadow). */
    lv_obj_t *info = ui_obj(g_np.main);
    lv_obj_set_pos(info, NP_INFO_X, NP_ART_Y - 6);
    lv_obj_set_size(info, NP_INFO_W, SCR_H - (NP_ART_Y - 6) - 8);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 12, 0);
    lv_obj_add_flag(info, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *now = ui_obj(info);
    lv_obj_set_size(now, NP_INFO_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(now, 12, 0);
    eq_create(&g_np.eq, now);
    g_np.state_lbl = ui_label(now, &font_title_28, C_HOT_INK);
    lv_label_set_text(g_np.state_lbl, "Now Playing");

    g_np.title = ui_label(info, &font_title_48, C_INK);
    lv_obj_set_width(g_np.title, NP_INFO_W);

    lv_obj_t *arow = ui_obj(info);
    lv_obj_set_size(arow, NP_INFO_W, ART_NP_SMALL);
    art_create(&g_np.artist_art, arow, ART_NP_SMALL, ART_LIST, 12);
    lv_obj_align(g_np.artist_art.box, LV_ALIGN_LEFT_MID, 0, 0);
    g_np.artist = ui_label(arow, &font_chat_36, C_INK);
    ui_label_one_line(g_np.artist, NP_INFO_W - ART_NP_SMALL - 16);
    lv_obj_align(g_np.artist, LV_ALIGN_LEFT_MID, ART_NP_SMALL + 16, 0);

    lv_obj_t *lrow = ui_obj(info);
    lv_obj_set_size(lrow, NP_INFO_W, ART_NP_SMALL);
    art_create(&g_np.album_art, lrow, ART_NP_SMALL, ART_LIST, 12);
    lv_obj_align(g_np.album_art.box, LV_ALIGN_LEFT_MID, 0, 0);
    g_np.album = ui_label(lrow, &font_chat_28, C_INK_2);
    ui_label_one_line(g_np.album, NP_INFO_W - ART_NP_SMALL - 16);
    lv_obj_align(g_np.album, LV_ALIGN_LEFT_MID, ART_NP_SMALL + 16, 0);

    g_np.count = ui_label(info, &font_chat_28, C_INK_2);

    g_np.bar = lv_bar_create(info);
    lv_obj_remove_style_all(g_np.bar);
    lv_obj_set_size(g_np.bar, NP_INFO_W, 14);
    lv_obj_set_style_radius(g_np.bar, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_np.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_np.bar, UI_C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_radius(g_np.bar, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_np.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_np.bar, C_HOT, LV_PART_INDICATOR);
    lv_bar_set_range(g_np.bar, 0, 1000);

    lv_obj_t *times = ui_obj(info);
    lv_obj_set_size(times, NP_INFO_W, LV_SIZE_CONTENT);
    g_np.elapsed = ui_label(times, &font_chat_28, C_INK_2);
    lv_obj_align(g_np.elapsed, LV_ALIGN_LEFT_MID, 0, 0);
    g_np.remaining = ui_label(times, &font_chat_28, C_INK_2);
    lv_obj_align(g_np.remaining, LV_ALIGN_RIGHT_MID, 0, 0);

    /* The keys in keyboard order: ← previous, Space play/pause, → next. */
    lv_obj_t *keys = ui_obj(info);
    lv_obj_set_size(keys, NP_INFO_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(keys, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(keys, 24, 0);
    lv_obj_set_style_pad_top(keys, 10, 0);
    lv_obj_set_style_pad_bottom(keys, 14, 0);   /* the Play sticker's ring and shadow */
    lv_obj_set_style_pad_hor(keys, 6, 0);
    lv_obj_add_flag(keys, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    np_button(keys, 88, false, LV_SYMBOL_PREV, NULL);
    np_button(keys, 112, true, LV_SYMBOL_PAUSE, &g_np.pp_sym);
    np_button(keys, 88, false, LV_SYMBOL_NEXT, NULL);

    lv_obj_add_flag(np, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Volume overlay ------------------------------------------------------ */

/* A 10×10 diagonal hatch, tiled over the segments above music_volume_max. */
static uint32_t    g_hatch_px[10 * 10];
static lv_image_dsc_t g_hatch;

static void hatch_init(void)
{
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++)
            g_hatch_px[y * 10 + x] = ((x + y) % 10) < 5 ? 0xFFC4CEDBu : 0xFFFFFFFFu;
    memset(&g_hatch, 0, sizeof(g_hatch));
    g_hatch.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_hatch.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    g_hatch.header.w      = 10;
    g_hatch.header.h      = 10;
    g_hatch.header.stride = 10 * 4;
    g_hatch.data_size     = sizeof(g_hatch_px);
    g_hatch.data          = (const uint8_t *)g_hatch_px;
}

static void vol_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    ui_set_hidden(g_vol.obj, true);
}

static void vol_create(lv_obj_t *parent)
{
    hatch_init();
    lv_obj_t *v = ui_obj(parent);
    g_vol.obj = v;
    lv_obj_set_size(v, 640, 220);
    lv_obj_center(v);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v, UI_C_SURFACE, 0);
    lv_obj_set_style_radius(v, UI_RADIUS, 0);
    ui_sticker(v, 4, 10);

    lv_obj_t *icon = ui_label(v, &lv_font_montserrat_48, C_HOT);
    lv_label_set_text(icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 36, 0);

    int x0 = 36 + 60 + 28, w = 640 - x0 - 36;
    g_vol.value = ui_label(v, &font_title_48, C_INK);
    lv_obj_set_pos(g_vol.value, x0, 18);
    g_vol.msg = ui_label(v, &font_title_28, C_HOT_INK);
    lv_obj_set_pos(g_vol.msg, x0 + 140, 32);

    int gap = 4, sw = (w - gap * (VOL_SEGS - 1)) / VOL_SEGS, segs_h = 46;
    for (int i = 0; i < VOL_SEGS; i++) {
        lv_obj_t *s = ui_obj(v);
        int h = segs_h * (30 + i * 35 / 10) / 100;
        lv_obj_set_size(s, sw, h);
        lv_obj_set_pos(s, x0 + i * (sw + gap), 84 + segs_h - h);
        lv_obj_set_style_radius(s, 3, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, UI_C_TRACK, 0);
        lv_obj_set_style_bg_image_tiled(s, true, 0);
        g_vol.seg[i] = s;
    }
    lv_obj_t *quiet = ui_label(v, &font_chat_28, C_INK_2);
    lv_label_set_text(quiet, "quiet");
    lv_obj_set_pos(quiet, x0, 84 + segs_h + 10);
    g_vol.cap = ui_label(v, &font_chat_28, C_INK_2);
    lv_obj_set_width(g_vol.cap, w - 100);
    lv_obj_set_style_text_align(g_vol.cap, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_vol.cap, x0 + 100, 84 + segs_h + 10);

    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
}

static void vol_show(int delta)
{
    if (!g_ops) return;
    int v = g_ops->volume_step(delta);
    int max = g_ops->volume_max();
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(g_vol.value, buf);

    const char *msg = "";
    if (delta > 0 && v >= max)      msg = "that's the loudest!";
    else if (v <= 0)                msg = "silent";
    else if (v >= max)              msg = "that's the loudest!";
    lv_label_set_text(g_vol.msg, msg);
    lv_obj_update_layout(g_vol.value);
    lv_obj_align_to(g_vol.msg, g_vol.value, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -6);

    for (int i = 0; i < VOL_SEGS; i++) {
        lv_obj_t *s = g_vol.seg[i];
        bool locked = i * PLAYER_VOLUME_STEP >= max;
        bool on = !locked && i < v / PLAYER_VOLUME_STEP;
        lv_obj_set_style_bg_image_src(s, locked ? &g_hatch : NULL, 0);
        lv_obj_set_style_bg_color(s, on ? C_HOT : UI_C_TRACK, 0);
    }
    snprintf(buf, sizeof(buf), "max %d%%", max);
    lv_label_set_text(g_vol.cap, buf);

    ui_set_hidden(g_vol.obj, false);
    lv_obj_move_foreground(g_vol.obj);
    lv_timer_reset(g_vol_timer);
    lv_timer_resume(g_vol_timer);
}

/* ---- "No music yet" ------------------------------------------------------ */

static void nomusic_create(lv_obj_t *parent)
{
    lv_obj_t *n = ui_obj(parent);
    g_nomusic = n;
    lv_obj_set_size(n, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(n, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(n, UI_C_BG, 0);

    /* The note on a round orange sticker. */
    lv_obj_t *badge = ui_obj(n);
    lv_obj_set_size(badge, 200, 200);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, -110);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge, C_HOT, 0);
    ui_sticker(badge, 4, 10);
    lv_obj_t *note = ui_label(badge, &font_icon_120, UI_C_ON_MUSIC);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_align(note, LV_ALIGN_CENTER, -4, 4);   /* optical centre of the note */

    lv_obj_t *msg = ui_label(n, &font_title_48, C_INK);
    lv_label_set_text(msg, "No music yet");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *sub = ui_label(n, &font_chat_28, C_INK_2);
    lv_label_set_text(sub, "A grown-up can put songs in the music folder.");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 140);
}

/* ---- Player-driven widgets ----------------------------------------------- */

static void np_set_art(void)
{
    if (g_view != MUSIC_VIEW_NOW_PLAYING) {
        g_np.art_pending = true;          /* decode the 512 cover when shown */
        return;
    }
    g_np.art_pending = false;
    art_set(&g_np.art, g_now.art_id, g_now.known ? g_now.album : g_now.title);
}

/* Texts and art of the strip and Now Playing for the current track. */
static void update_now_widgets(void)
{
    if (!g_strip.obj) return;
    bool idle = !g_now.valid;
    ui_set_hidden(g_strip.live, idle);
    ui_set_hidden(g_strip.idle_icon, !idle);
    ui_set_hidden(g_strip.idle_t1, !idle);
    ui_set_hidden(g_strip.idle_t2, !idle);
    eq_show(&g_strip.eq, !idle);
    eq_show(&g_np.eq, !idle);
    if (!idle) {
        lv_label_set_text(g_strip.title, g_now.title);
        char sub[2 * LIBRARY_NAME_LEN + 8];
        if (g_now.known)
            snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", g_now.artist, g_now.album);
        else
            sub[0] = 0;
        lv_label_set_text(g_strip.sub, sub);
        art_set(&g_strip.art, g_now.art_id, g_now.known ? g_now.album : g_now.title);

        label_set_lines(g_np.title, g_now.title, NP_INFO_W, 2);
        lv_label_set_text(g_np.artist, g_now.artist);
        lv_label_set_text(g_np.album, g_now.album);
        art_set(&g_np.artist_art, g_now.artist_art_id, g_now.artist);
        art_set(&g_np.album_art, g_now.art_id, g_now.known ? g_now.album : g_now.title);
        char cnt[64];
        int qi = g_ops ? g_ops->current_index() : -1, qn = g_ops ? g_ops->queue_len() : 0;
        if (qi >= 0 && qn > 0) snprintf(cnt, sizeof(cnt), "Song %d of %d", qi + 1, qn);
        else cnt[0] = 0;
        lv_label_set_text(g_np.count, cnt);
        np_set_art();
    }
    update_position(true);
}

static void update_state_widgets(void)
{
    if (!g_strip.obj) return;
    bool playing = g_pstate == PLAYER_PLAYING;
    /* The glyph shows what Space would do. */
    lv_label_set_text(g_strip.state_sym, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(g_np.pp_sym, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(g_np.state_lbl, playing ? "Now Playing" : "Paused");
    ui_set_hidden(g_np.badge, playing || !g_now.valid);
    eq_timer_update();
    /* Every track and state change ends here: the live tile and our peek
     * pill in chat may change. */
    if (g_tile_listener) g_tile_listener();
}

static void update_position(bool force)
{
    if (!g_strip.obj) return;
    int64_t pos = g_ops && g_now.valid ? g_ops->position_ms() : 0;
    int64_t dur = g_ops && g_now.valid ? g_ops->duration_ms() : -1;
    if (dur <= 0) dur = g_now.duration_ms;
    if (pos < 0) pos = 0;
    if (dur > 0 && pos > dur) pos = dur;
    int prog = dur > 0 ? (int)(pos * 1000 / dur) : 0;
    int sec = (int)(pos / 1000), total = dur > 0 ? (int)(dur / 1000) : -1;

    if (force || prog != g_last_prog) {
        g_last_prog = prog;
        lv_bar_set_value(g_strip.line, prog, LV_ANIM_OFF);
        lv_bar_set_value(g_np.bar, prog, LV_ANIM_OFF);
    }
    if (force || sec != g_last_sec || total != g_last_total) {
        g_last_sec = sec;
        g_last_total = total;
        char el[16], tot[16], rem[20], buf[40];
        fmt_time(el, sizeof(el), pos);
        if (dur > 0) {
            fmt_time(tot, sizeof(tot), dur);
            snprintf(buf, sizeof(buf), "%s / %s", el, tot);
            fmt_time(tot, sizeof(tot), dur - pos);
            snprintf(rem, sizeof(rem), "-%s", tot);
        } else {
            snprintf(buf, sizeof(buf), "%s", el);
            rem[0] = 0;
        }
        lv_label_set_text(g_strip.time, g_now.valid ? buf : "");
        lv_label_set_text(g_np.elapsed, el);
        lv_label_set_text(g_np.remaining, rem);
    }
}

/* Re-read the current track and redraw everything that marks it. */
static void track_changed(void)
{
    refresh_now();
    update_now_widgets();
    update_state_widgets();
    if (g_view == MUSIC_VIEW_BROWSE) render_all();
}

static void cb_track(void *ud)    { (void)ud; if (g_root) track_changed(); }
static void cb_state(void *ud)
{
    (void)ud;
    if (!g_root || !g_ops) return;
    g_pstate = g_ops->state();
    update_state_widgets();
    update_position(true);
}
static void cb_position(void *ud) { (void)ud; if (g_root) update_position(false); }
static void cb_volume(void *ud)   { (void)ud; }

/* Poll the player's state right after an operation of ours, so the UI
 * doesn't wait for the next tick's callbacks (which will say the same). */
static void sync_player(void)
{
    if (!g_ops) return;
    int64_t id = g_ops->current_id();
    g_pstate = g_ops->state();
    if (id != g_now.id || (id == 0) != !g_now.valid) {
        track_changed();
    } else {
        update_state_widgets();
        update_position(true);      /* e.g. ← or Enter restarted the track */
    }
}

/* ---- Live tile (T9) ------------------------------------------------------ */

static void tile_delete_cb(lv_event_t *e)
{
    (void)e;
    memset(&g_tile, 0, sizeof(g_tile));
}

static void tile_build(lv_obj_t *face)
{
    memset(&g_tile, 0, sizeof(g_tile));
    g_tile.w = lv_obj_get_style_width(face, 0);
    g_tile.obj = ui_obj(face);
    lv_obj_set_size(g_tile.obj, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(g_tile.obj, tile_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(g_tile.obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    g_tile.live = ui_obj(g_tile.obj);
    lv_obj_set_size(g_tile.live, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(g_tile.live, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    art_create(&g_tile.art, g_tile.live, TILE_ART, TILE_ART, 22);
    lv_obj_set_pos(g_tile.art.box, TILE_INSET, TILE_INSET);
    ui_sticker(g_tile.art.box, 4, 0);

    /* The state glyph: what the player is doing (not what a key would do,
     * unlike the strip's glyph: nothing on the switcher plays or pauses).
     * A white sticker. */
    g_tile.glyph_bg = ui_obj(g_tile.live);
    lv_obj_set_size(g_tile.glyph_bg, TILE_GLYPH, TILE_GLYPH);
    if (g_tile.w >= TILE_INSET + TILE_ART + 32 + TILE_GLYPH)
        lv_obj_set_pos(g_tile.glyph_bg, TILE_INSET + TILE_ART + 32, TILE_INSET + (TILE_ART - TILE_GLYPH) / 2);
    else    /* a narrow tile (three apps): on the cover's bottom-right corner */
        lv_obj_set_pos(g_tile.glyph_bg, g_tile.w - TILE_GLYPH - TILE_INSET,
                       TILE_INSET + TILE_ART - TILE_GLYPH * 2 / 3);
    lv_obj_set_style_radius(g_tile.glyph_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_tile.glyph_bg, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(g_tile.glyph_bg, LV_OPA_COVER, 0);
    ui_sticker(g_tile.glyph_bg, 4, 0);
    g_tile.glyph = ui_label(g_tile.glyph_bg, &lv_font_montserrat_48, C_INK);
    lv_obj_center(g_tile.glyph);

    g_tile.title = ui_label(g_tile.live, &font_title_36, UI_C_ON_MUSIC);
    lv_obj_set_pos(g_tile.title, TILE_INSET, TILE_INSET + TILE_ART + 16);
    lv_obj_set_width(g_tile.title, g_tile.w);

    /* Nothing loaded: a big plain note in the middle of the face. */
    g_tile.idle = ui_label(g_tile.obj, &font_icon_120, UI_C_ON_MUSIC);
    lv_label_set_text(g_tile.idle, LV_SYMBOL_AUDIO);
    lv_obj_center(g_tile.idle);
}

void ui_music_tile_render(lv_obj_t *face)
{
    if (!face) return;
    if (!g_tile.obj || lv_obj_get_parent(g_tile.obj) != face) {
        if (g_tile.obj) lv_obj_delete(g_tile.obj);
        tile_build(face);
    }

    bool idle = !g_now.valid;
    ui_set_hidden(g_tile.live, idle);
    ui_set_hidden(g_tile.idle, !idle);
    if (idle) return;

    /* All cheap when nothing changed: art_set skips the same art, LVGL
     * skips setting a label to the text it has. */
    art_set(&g_tile.art, g_now.art_id, g_now.known ? g_now.album : g_now.title);
    const char *sym = g_pstate == PLAYER_PLAYING ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
    if (strcmp(lv_label_get_text(g_tile.glyph), sym) != 0)
        lv_label_set_text(g_tile.glyph, sym);
    if (strcmp(lv_label_get_text(g_tile.title), g_now.title) != 0)
        label_set_lines(g_tile.title, g_now.title, g_tile.w, 2);
}

void ui_music_set_tile_listener(void (*cb)(void))
{
    g_tile_listener = cb;
}

/* ---- Peek: our news in chat's header ------------------------------------- */

static void peek_delete_cb(lv_event_t *e)
{
    (void)e;
    memset(&g_peek_ui, 0, sizeof(g_peek_ui));
}

bool ui_music_peek_render(lv_obj_t *pill)
{
    if (!g_now.valid) return false;
    if (!g_peek_ui.text || lv_obj_get_parent(g_peek_ui.text) != pill) {
        lv_obj_clean(pill);
        lv_obj_set_style_pad_left(pill, 16, 0);
        /* A still equaliser: the pill is small and repaints with the
         * header; the animated ones are music's own. */
        g_peek_ui.eq = ui_obj(pill);
        lv_obj_set_size(g_peek_ui.eq, 22, 22);
        static const uint8_t h[3] = { 9, 22, 14 };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = ui_obj(g_peek_ui.eq);
            lv_obj_set_size(b, 6, h[i]);
            lv_obj_set_style_radius(b, 2, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(b, C_HOT, 0);
            lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 8, 0);
        }
        g_peek_ui.text = ui_label(pill, &font_title_28, C_HOT_INK);
        lv_label_set_long_mode(g_peek_ui.text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(g_peek_ui.text, 320, 0);
        lv_obj_add_event_cb(g_peek_ui.text, peek_delete_cb, LV_EVENT_DELETE, NULL);
    }
    if (strcmp(lv_label_get_text(g_peek_ui.text), g_now.title) != 0)
        lv_label_set_text(g_peek_ui.text, g_now.title);
    return true;
}

lv_obj_t *ui_music_peek_pill(void)
{
    return g_peek;
}

/* ---- Views --------------------------------------------------------------- */

static void set_view(music_view_t v)
{
    g_view = v;
    ui_set_hidden(g_nomusic, v != MUSIC_VIEW_NO_MUSIC);
    ui_set_hidden(g_cols, v != MUSIC_VIEW_BROWSE);
    ui_set_hidden(g_strip.obj, v != MUSIC_VIEW_BROWSE);
    ui_set_hidden(g_np.obj, v != MUSIC_VIEW_NOW_PLAYING);
    if (v == MUSIC_VIEW_NOW_PLAYING && g_np.art_pending) np_set_art();
    if (v == MUSIC_VIEW_BROWSE) render_all();
    eq_timer_update();
}

music_view_t ui_music_view(void)
{
    return g_view;
}

key_ctx_t ui_music_key_ctx(void)
{
    switch (g_view) {
    case MUSIC_VIEW_BROWSE:      return KEY_CTX_MUSIC_BROWSE;
    case MUSIC_VIEW_NOW_PLAYING: return KEY_CTX_NOW_PLAYING;
    default:                     return KEY_CTX_NO_MUSIC;
    }
}

/* ---- Library reload (§7.7) ----------------------------------------------- */

static void clear_lists(void)
{
    g_albums.count = g_albums.total = 0;
    g_tracks.count = g_tracks.total = 0;
    g_tracks_of = -1;
    g_sel[0] = g_sel[1] = -1;
}

void ui_music_reload(void)
{
    if (!g_root) return;
    bool ready = g_lib && library_state(g_lib) == LIBRARY_READY;
    if (ready && library_load_all_albums(g_lib, &g_albums) != LIBRARY_OK) {
        /* Busy or broken: keep what is shown; the next poll retries. */
        if (g_view != MUSIC_VIEW_NO_MUSIC) return;
        ready = false;
    }
    if (!ready || g_albums.count == 0) {
        clear_lists();
        refresh_now();
        update_now_widgets();
        update_state_widgets();
        set_view(MUSIC_VIEW_NO_MUSIC);
        return;
    }

    /* An art id can be reused for new bytes after a pass: forget decoded
     * images (rare: only when the library changed). */
    lv_image_cache_drop(NULL);
    lv_image_header_cache_drop(NULL);
    for (int j = 0; j < g_grid.pool; j++) {
        art_unbind(&g_grid.cards[j].art);
        g_grid.cards[j].item = -1;
    }
    for (int j = 0; j < g_page.pool; j++) {
        art_unbind(&g_page.rows[j].art);
        g_page.rows[j].item = -1;
    }
    art_unbind(&g_page.art);
    art_unbind(&g_strip.art);
    art_unbind(&g_np.art);
    art_unbind(&g_np.artist_art);
    art_unbind(&g_np.album_art);
    art_unbind(&g_tile.art);

    bool first = (g_view == MUSIC_VIEW_NO_MUSIC);
    g_list_gen[MUSIC_COL_ALBUMS]++;
    refresh_now();
    g_force_load = true;
    int ai = library_all_albums_find(&g_albums, g_album_sel_id, g_sel[0] < 0 ? 0 : g_sel[0]);
    select_album(ai);
    g_force_load = false;
    if (first) g_focus = MUSIC_COL_ALBUMS;

    update_now_widgets();
    update_state_widgets();
    set_view(first ? MUSIC_VIEW_BROWSE : g_view);
    render_all();
}

void ui_music_poll_library(void)
{
    if (g_lib && library_poll(g_lib)) ui_music_reload();
}

/* ---- Play (§7.3) --------------------------------------------------------- */

/* Resolve paths for `items` into g_q (skipping tracks the library can't
 * give a path for). *start is updated to the index of items[want] in g_q
 * (or the next playable one). Returns the queue length. */
static int build_queue(const library_track_t *items, int count, int want, int *start)
{
    int n = 0;
    size_t used = 0;
    *start = 0;
    bool found = false;
    for (int i = 0; i < count && n < PLAYER_MAX_QUEUE; i++) {
        if (library_track_get(g_lib, items[i].id, &g_info) != LIBRARY_OK || !g_info.path[0])
            continue;
        size_t len = strlen(g_info.path) + 1;
        if (used + len > sizeof(g_qpaths)) break;
        memcpy(g_qpaths + used, g_info.path, len);
        g_q[n].id = items[i].id;
        g_q[n].path = g_qpaths + used;
        used += len;
        if (!found && i >= want) {
            *start = n;
            found = true;
        }
        n++;
    }
    return n;
}

/* ---- Keys ---------------------------------------------------------------- */

static void open_np(void)
{
    set_view(MUSIC_VIEW_NOW_PLAYING);
    update_now_widgets();
    update_state_widgets();
}

/* The list a bounce moves: the grid or the songs. */
static lv_obj_t *bounce_target(void)
{
    return g_focus == MUSIC_COL_ALBUMS ? g_grid.list : g_page.list;
}

/* Enter on a song (§7.3): play, then open Now Playing so ←/→ skip tracks at
 * once: the album from that song, from 0:00, even if it is the current
 * song (Enter always plays from the start). Nothing playable (no paths):
 * bounce, stay on the album page. */
static void play_and_open(void)
{
    if (!g_ops || !g_lib || g_sel[1] < 0) {
        bounce(bounce_target(), MUSIC_BOUNCE_RIGHT);
        return;
    }
    int start = 0;
    int n = build_queue(g_tracks.items, g_tracks.count, g_sel[1], &start);
    if (n <= 0) {
        bounce(bounce_target(), MUSIC_BOUNCE_RIGHT);
        return;
    }
    g_ops->play_queue(g_q, n, start);
    sync_player();
    if (g_ops->current_id() > 0) open_np();
}

/* Leave Now Playing for the browse view, with the playing album and song
 * selected (§7.4, D4): `focus` SONGS = its album page (Esc), ALBUMS = the
 * grid (Win). */
static void close_np(int focus)
{
    if (g_now.known) {
        int ai = library_all_albums_find(&g_albums, g_now.album_id, g_sel[0]);
        if (ai >= 0 && g_albums.items[ai].id == g_now.album_id)
            select_album(ai);           /* lands on the playing song */
    }
    g_focus = focus;
    set_view(MUSIC_VIEW_BROWSE);
}

/* The grid: arrows move between cards (edges bounce, no wrap; ↓ into a
 * shorter last row lands on its last card); Enter opens the album's page. */
static void key_grid(uint32_t key)
{
    int n = g_albums.count, sel = g_sel[0];
    if (sel < 0) {
        if (key != KF_KEY_MENU) bounce(g_grid.list, MUSIC_BOUNCE_RIGHT);
        else if (g_ops && g_ops->current_id() > 0) open_np();
        return;
    }
    int ni = -1, kind = MUSIC_BOUNCE_NONE;
    switch (key) {
    case LV_KEY_LEFT:
        if (sel % GRID_COLS == 0) kind = MUSIC_BOUNCE_LEFT; else ni = sel - 1;
        break;
    case LV_KEY_RIGHT:
        if (sel % GRID_COLS == GRID_COLS - 1 || sel + 1 >= n) kind = MUSIC_BOUNCE_RIGHT;
        else ni = sel + 1;
        break;
    case LV_KEY_UP:
        if (sel - GRID_COLS < 0) kind = MUSIC_BOUNCE_UP; else ni = sel - GRID_COLS;
        break;
    case LV_KEY_DOWN:
        if (sel + GRID_COLS < n) ni = sel + GRID_COLS;
        else if (sel / GRID_COLS < (n - 1) / GRID_COLS) ni = n - 1;
        else kind = MUSIC_BOUNCE_DOWN;
        break;
    case LV_KEY_ENTER:
        /* Into the album: the playing song overrides the selection (§7.4). */
        {
            int p = playing_song_index();
            if (p >= 0) select_song(p);
        }
        g_focus = MUSIC_COL_SONGS;
        render_all();
        return;
    case KF_KEY_MENU:
        /* Win: over to the player. Nothing loaded: bounce. */
        if (g_ops && g_ops->current_id() > 0) open_np();
        else bounce(g_grid.list, MUSIC_BOUNCE_RIGHT);
        return;
    default:
        return;     /* Tab, letters, digits, Backspace: ignored */
    }
    if (kind != MUSIC_BOUNCE_NONE) {
        bounce(g_grid.list, kind);
        return;
    }
    select_album(ni);
    render_grid();
}

/* The album page: ↑/↓ songs, Enter plays, ← back to the grid, → bounces. */
static void key_page(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_DOWN: {
        int d = key == LV_KEY_UP ? -1 : 1;
        int ni = g_sel[1] + d;
        if (g_sel[1] < 0 || ni < 0 || ni >= g_tracks.count) {
            bounce(g_page.list, d < 0 ? MUSIC_BOUNCE_UP : MUSIC_BOUNCE_DOWN);
            return;
        }
        select_song(ni);
        render_page();
        return;
    }
    case LV_KEY_RIGHT:
        bounce(g_page.list, MUSIC_BOUNCE_RIGHT);   /* → does nothing; Enter plays */
        return;
    case LV_KEY_LEFT:
        g_focus = MUSIC_COL_ALBUMS;
        render_all();
        return;
    case LV_KEY_ENTER:
        play_and_open();
        return;
    case KF_KEY_MENU:
        if (g_ops && g_ops->current_id() > 0) open_np();
        else bounce(g_page.list, MUSIC_BOUNCE_RIGHT);
        return;
    default:
        return;
    }
}

static void key_now_playing(uint32_t key)
{
    switch (key) {
    case LV_KEY_LEFT:                   /* iPod rule (player_prev) */
        if (g_ops) { g_ops->prev(); sync_player(); }
        return;
    case LV_KEY_RIGHT:                  /* last track: bounce, plays on */
        if (!g_ops || !g_ops->next()) bounce(g_np.art.box, MUSIC_BOUNCE_RIGHT);
        else sync_player();
        return;
    case LV_KEY_UP:                     /* volume, repeats while held */
    case LV_KEY_DOWN:
        vol_show(key == LV_KEY_UP ? PLAYER_VOLUME_STEP : -PLAYER_VOLUME_STEP);
        return;
    case LV_KEY_ENTER:                  /* the song again from 0:00, playing */
        if (g_ops) { g_ops->restart(); sync_player(); }
        return;
    case KF_KEY_MENU:                   /* Win: back to the album grid */
        close_np(MUSIC_COL_ALBUMS);
        return;
    default:
        return;
    }
}

void ui_music_handle_key(uint32_t key)
{
    if (!g_root) return;
    /* Every view: Space, PgUp, PgDn (§8.2–§8.4). Space only pauses or
     * resumes: stopped or nothing loaded → nothing. */
    if (key == KF_KEY_SPACE) {
        if (g_ops) { g_ops->toggle(); sync_player(); }
        return;
    }
    if (key == KF_KEY_PGUP || key == KF_KEY_PGDN) {
        vol_show(key == KF_KEY_PGUP ? PLAYER_VOLUME_STEP : -PLAYER_VOLUME_STEP);
        return;
    }
    if (g_view == MUSIC_VIEW_BROWSE) {
        if (g_focus == MUSIC_COL_ALBUMS) key_grid(key);
        else                             key_page(key);
    } else if (g_view == MUSIC_VIEW_NOW_PLAYING) {
        key_now_playing(key);
    }
    /* NO_MUSIC: everything else is ignored (§8.4). */
}

bool ui_music_handle_esc(void)
{
    if (!g_root) return false;
    if (g_view == MUSIC_VIEW_NOW_PLAYING) {
        close_np(MUSIC_COL_SONGS);
        return true;
    }
    if (g_view == MUSIC_VIEW_BROWSE && g_focus == MUSIC_COL_SONGS) {
        g_focus = MUSIC_COL_ALBUMS;     /* the album page → the grid */
        render_all();
        return true;
    }
    return false;
}

/* ---- Key hints (CHATBOX_KEY_HINTS) --------------------------------------- */

static const ui_hint_t k_hints_grid[] = {
    { { UI_KEY_LEFT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_RIGHT }, "choose", false },
    { { "Enter" }, "open", false },
    { { "Space" }, "pause", false },
    { { "PgUp", "PgDn" }, "volume", false },
    { { "Esc" }, "home", true },
};
static const ui_hint_t k_hints_page[] = {
    { { UI_KEY_UP, UI_KEY_DOWN }, "choose", false },
    { { "Enter" }, "play", false },
    { { "Space" }, "pause", false },
    { { "PgUp", "PgDn" }, "volume", false },
    { { "Esc" }, "albums", true },
};
static const ui_hint_t k_hints_np[] = {
    { { UI_KEY_LEFT }, "back", false },
    { { "Space" }, "pause", false },
    { { UI_KEY_RIGHT }, "next", false },
    { { UI_KEY_UP, UI_KEY_DOWN }, "volume", false },
    { { "Win" }, "albums", false },
    { { "Esc" }, "album", true },
};
static const ui_hint_t k_hints_none[] = {
    { { "PgUp", "PgDn" }, "volume", false },
    { { "Esc" }, "home", true },
};
#define N_HINTS(a) ((int)(sizeof(a) / sizeof((a)[0])))

const ui_hint_t *ui_music_hints(int *n)
{
    switch (g_view) {
    case MUSIC_VIEW_NOW_PLAYING: *n = N_HINTS(k_hints_np); return k_hints_np;
    case MUSIC_VIEW_BROWSE:
        if (g_focus == MUSIC_COL_SONGS) { *n = N_HINTS(k_hints_page); return k_hints_page; }
        *n = N_HINTS(k_hints_grid);
        return k_hints_grid;
    default:                     *n = N_HINTS(k_hints_none); return k_hints_none;
    }
}

/* ---- Lifecycle ----------------------------------------------------------- */

static void root_delete_cb(lv_event_t *e)
{
    (void)e;
    if (g_eq_timer)    lv_timer_delete(g_eq_timer);
    if (g_vol_timer)   lv_timer_delete(g_vol_timer);
    g_eq_timer = g_vol_timer = NULL;
    g_eq_running = false;
    g_root = NULL;
    g_cols = NULL;
    g_head_lbl = NULL;
    g_peek = NULL;
    g_nomusic = NULL;
    memset(&g_grid, 0, sizeof(g_grid));
    memset(&g_page, 0, sizeof(g_page));
    memset(&g_strip, 0, sizeof(g_strip));
    memset(&g_np, 0, sizeof(g_np));
    memset(&g_vol, 0, sizeof(g_vol));
    g_visible = false;
}

void ui_music_create(lv_obj_t *root)
{
    g_root = root;
    g_view = MUSIC_VIEW_NO_MUSIC;
    g_focus = MUSIC_COL_ALBUMS;
    g_visible = false;
    clear_lists();
    g_album_sel_id = 0;
    memset(g_album_mem, 0, sizeof(g_album_mem));
    g_album_mem_next = 0;
    memset(&g_now, 0, sizeof(g_now));
    g_pstate = PLAYER_IDLE;
    g_last_sec = g_last_prog = -1;
    g_last_total = -2;
#ifdef CHATBOX_TESTING
    g_bounces = 0;
    g_last_bounce = MUSIC_BOUNCE_NONE;
#endif

    lv_obj_set_style_bg_color(root, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);

    browse_create(root);
    strip_create(root);
    np_create(root);
    nomusic_create(root);
    vol_create(root);

    g_eq_timer = lv_timer_create(eq_timer_cb, EQ_STEP_MS, NULL);
    lv_timer_pause(g_eq_timer);
    g_vol_timer = lv_timer_create(vol_timer_cb, VOL_SHOW_MS, NULL);
    lv_timer_pause(g_vol_timer);

    set_view(MUSIC_VIEW_NO_MUSIC);
    ui_music_reload();
}

void ui_music_set_sources(library_t *lib, const music_player_ops_t *ops)
{
    g_lib = lib;
    g_ops = ops;
    if (ops) {
        static const player_callbacks_t cb = {
            .track_changed    = cb_track,
            .state_changed    = cb_state,
            .volume_changed   = cb_volume,
            .position_changed = cb_position,
        };
        ops->set_callbacks(&cb);
        g_pstate = ops->state();
    } else {
        g_pstate = PLAYER_IDLE;
    }
    if (g_root) ui_music_reload();
}

void ui_music_on_show(void)
{
    g_visible = true;
    eq_timer_update();
}

void ui_music_on_hide(void)
{
    g_visible = false;
    eq_timer_update();
}

/* ---- The real player ----------------------------------------------------- */

const music_player_ops_t music_player_real_ops = {
    .set_callbacks = player_set_callbacks,
    .play_queue    = player_play_queue,
    .toggle        = player_toggle,
    .restart       = player_restart,
    .prev          = player_prev,
    .next          = player_next,
    .volume_step   = player_volume_step,
    .state         = player_state,
    .current_id    = player_current_id,
    .current_path  = player_current_path,
    .current_index = player_current_index,
    .queue_len     = player_queue_len,
    .position_ms   = player_position_ms,
    .duration_ms   = player_duration_ms,
    .volume        = player_volume,
    .volume_max    = player_volume_max,
};

/* ---- Test hooks ---------------------------------------------------------- */

#ifdef CHATBOX_TESTING
static bool is_hidden(lv_obj_t *o)
{
    return !o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void copy_label(char *dst, size_t n, lv_obj_t *l)
{
    snprintf(dst, n, "%s", l ? lv_label_get_text(l) : "");
}

/* One "row" of either list for the state below: a card or a song row. */
static void observe(music_test_state_t *o, int ci, lv_obj_t *obj, int item, const eq_t *eq,
                    const art_t *art)
{
    if (!is_hidden(obj)) o->rows_visible[ci]++;
    if (item < 0) return;
    o->rows_bound[ci]++;
    if (eq->shown) {
        o->marked_rows[ci]++;
        o->marked_id[ci] = ci == MUSIC_COL_ALBUMS ? g_albums.items[item].id
                                                  : g_tracks.items[item].id;
    }
    if (item != g_sel[ci]) return;
    o->sel_row_fallback[ci] = art->fallback && !is_hidden(art->box);
    o->sel_row_image[ci] = !art->fallback && !is_hidden(art->img);
    o->sel_row_focused[ci] = lv_obj_get_style_outline_width(obj, 0) > 0;
    if (art->fallback)
        snprintf(o->sel_row_letter[ci], sizeof(o->sel_row_letter[ci]), "%s",
                 lv_label_get_text(art->letter));
}

void ui_music_test_state(music_test_state_t *o)
{
    memset(o, 0, sizeof(*o));
    o->view = g_view;
    o->focus_col = g_focus;
    o->count[MUSIC_COL_ALBUMS] = g_albums.count;
    o->count[MUSIC_COL_SONGS] = g_tracks.count;
    o->sel[MUSIC_COL_ALBUMS] = g_sel[0];
    o->sel[MUSIC_COL_SONGS] = g_sel[1];
    o->sel_id[MUSIC_COL_ALBUMS] = sel_album_id();
    o->sel_id[MUSIC_COL_SONGS] = sel_track_id();
    o->row_widgets[MUSIC_COL_ALBUMS] = g_grid.pool;
    o->row_widgets[MUSIC_COL_SONGS] = g_page.pool;
    o->grid_visible = !is_hidden(g_grid.view) && !is_hidden(g_cols);
    o->page_visible = !is_hidden(g_page.obj) && !is_hidden(g_cols);
    for (int j = 0; j < g_grid.pool; j++) {
        card_t *c = &g_grid.cards[j];
        observe(o, MUSIC_COL_ALBUMS, c->obj, c->item, &c->eq, &c->art);
    }
    for (int j = 0; j < g_page.pool; j++) {
        row_t *r = &g_page.rows[j];
        observe(o, MUSIC_COL_SONGS, r->obj, r->item, &r->eq, &r->art);
    }
    copy_label(o->page_title, sizeof(o->page_title), g_page.title);
    o->peek_visible = !is_hidden(g_peek);
    o->bounces = g_bounces;
    o->last_bounce = g_last_bounce;
    o->eq_animating = g_eq_timer && g_eq_running;

    o->strip_visible = !is_hidden(g_strip.obj);
    o->strip_idle = is_hidden(g_strip.live);
    copy_label(o->strip_title, sizeof(o->strip_title), g_strip.title);
    copy_label(o->strip_sub, sizeof(o->strip_sub), g_strip.sub);
    copy_label(o->strip_time, sizeof(o->strip_time), g_strip.time);
    o->strip_fallback = g_strip.art.fallback;

    copy_label(o->np_state, sizeof(o->np_state), g_np.state_lbl);
    copy_label(o->np_title, sizeof(o->np_title), g_np.title);
    copy_label(o->np_artist, sizeof(o->np_artist), g_np.artist);
    copy_label(o->np_album, sizeof(o->np_album), g_np.album);
    copy_label(o->np_count, sizeof(o->np_count), g_np.count);
    copy_label(o->np_elapsed, sizeof(o->np_elapsed), g_np.elapsed);
    copy_label(o->np_remaining, sizeof(o->np_remaining), g_np.remaining);
    o->np_progress = g_np.bar ? lv_bar_get_value(g_np.bar) : 0;
    o->np_fallback = g_np.art.bound && g_np.art.fallback;
    o->np_image = g_np.art.bound && !g_np.art.fallback;
    o->np_paused_badge = !is_hidden(g_np.badge);

    o->vol_visible = !is_hidden(g_vol.obj);
    copy_label(o->vol_value, sizeof(o->vol_value), g_vol.value);
    copy_label(o->vol_msg, sizeof(o->vol_msg), g_vol.msg);
    for (int i = 0; i < VOL_SEGS; i++) {
        if (!g_vol.seg[i]) continue;
        if (lv_obj_get_style_bg_image_src(g_vol.seg[i], 0)) o->vol_segs_locked++;
        else if (lv_color_eq(lv_obj_get_style_bg_color(g_vol.seg[i], 0), C_HOT))
            o->vol_segs_on++;
    }

    o->tile_built = g_tile.obj != NULL;
    if (o->tile_built) {
        o->tile_idle = is_hidden(g_tile.live) && !is_hidden(g_tile.idle);
        copy_label(o->tile_title, sizeof(o->tile_title), g_tile.title);
        copy_label(o->tile_glyph, sizeof(o->tile_glyph), g_tile.glyph);
        o->tile_fallback = g_tile.art.bound && g_tile.art.fallback;
        o->tile_image = g_tile.art.bound && !g_tile.art.fallback && !is_hidden(g_tile.art.img);
    }
}
#endif
