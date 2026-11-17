/**
 * ui_photos.c — Photos app UI (see ui_photos.h; docs/photos.md).
 *
 * Layout (1280×UI_APP_H, docs/design.md)
 *   header   68 px: a picture glyph, "Photos" in Photos' ink green and, on
 *            the grid, "N photos".
 *   GRID     cards four across like music's album grid: a 256×192 picture
 *            (the photo centre-cropped to 4:3, rounded, unframed) and its
 *            date. The selected card wears the focus look (ui_theme.h).
 *   VIEWER   the photo, whole, scaled to fit 1168×592 (ink ring + solid
 *            shadow: one big sticker), round ← / → arrows at the sides
 *            (pale at the ends), and a bar with the date and "N of M".
 *   EMPTY    "No photos yet" with a green picture sticker.
 *   confirm  "Delete this photo?" on a white sticker, over grid or viewer,
 *            with [Enter] Delete and [Esc] Keep.
 *
 * Pictures (memory and drawing cost, see ui_photo_img.h)
 *   - Nothing here hands a photo file to LVGL. photo_decode() makes each
 *     picture at the size it is drawn, into malloc'd memory: 256×192 for a
 *     card (147 KB), at most 1168×592 for the viewer (~2 MB). LVGL's heap
 *     and image cache never see a 12 MP photo.
 *   - Cards are virtualised (a pool of the rows that fit plus one, re-bound
 *     as the grid scrolls). Card pictures live in a small cache of decoded
 *     thumbnails (THUMB_CACHE, keyed by name + mtime + size), so scrolling
 *     back is free. A card that has no thumbnail yet shows the soft green
 *     box; a timer decodes one card per tick, the selected one first, so a
 *     key press never waits for a row of photos.
 *   - The viewer decodes its photo on the key press that shows it (one
 *     photo, the one asked for), into the second of two buffers, then
 *     swaps, so the old picture stays up until the new one is ready.
 *   - A photo that doesn't decode (not really a JPEG/PNG, still being
 *     copied in, too big) shows the placeholder with a picture glyph.
 *   - No slide or zoom animations; the only animation is the small edge
 *     bounce (a translate of the grid or the viewer's photo).
 *
 * Deleting: Delete → the confirm sticker; Enter → photo_lib_delete()
 * (unlink, rescan), then the grid keeps the same index (clamped) and the
 * viewer shows the next photo (the previous when it was the last, the grid
 * when none are left). Chat is not touched: its bubble keeps the picture
 * it shows (photo_inbox's seen list keeps the photo from coming back).
 */

#include "ui_photos.h"
#include "ui_photo_img.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Look ---------------------------------------------------------------- */

#define SCR_W          UI_W
#define SCR_H          UI_APP_H
#define HEAD_H         UI_HEAD_H
#define HEAD_X         40
#define VIEW_H         (SCR_H - HEAD_H)

#define GRID_COLS       4
#define CARD_W        280
#define CARD_GAP_X     26
#define CARD_GAP_Y     22
#define CARD_PAD       12
#define PIC_W         256
#define PIC_H         192
#define PIC_RADIUS     16
#define GRID_X        ((SCR_W - GRID_COLS * CARD_W - (GRID_COLS - 1) * CARD_GAP_X) / 2)
#define GRID_PAD_Y     14
#define CARD_POOL      16
#define THUMB_CACHE    32

#define BAR_H         112
#define BIG_W        1168
#define BIG_H         592
#define BIG_RADIUS     30
#define BIG_RING        4
#define BIG_SHADOW     10
#define AREA_Y        (HEAD_H + 4)
#define AREA_H        (SCR_H - HEAD_H - BAR_H - 4)
#define ARROW          84
#define ARROW_X        20

#define LOAD_MS        10

/* The switcher tile. */
#define TILE_INSET      6

/* ---- State --------------------------------------------------------------- */

typedef struct {
    bool           used;
    char           name[PHOTO_NAME_MAX];
    time_t         mtime;
    int64_t        size;
    ui_photo_img_t img;
    bool           failed;
    uint32_t       lru;
} thumb_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *pic;          /* the soft box: placeholder and clip */
    lv_obj_t *img;
    lv_obj_t *icon;         /* can't-show glyph */
    lv_obj_t *cap;
    int       item;         /* bound photo, -1 = none */
    unsigned  gen;
    thumb_t  *th;           /* what the picture shows; NULL = loading */
    bool      pending;      /* waiting for its thumbnail */
} card_t;

static lv_obj_t *g_root;
static lv_obj_t *g_head, *g_head_sub;
static lv_obj_t *g_empty;

static struct {
    lv_obj_t *view;
    lv_obj_t *list;         /* holds the cards; translated by the bounce */
    card_t    cards[CARD_POOL];
    int       pool;
    int       card_h;
} g_grid;

static struct {
    lv_obj_t      *obj;
    lv_obj_t      *frame;   /* the sticker the photo sits in */
    lv_obj_t      *img;
    lv_obj_t      *fail;    /* placeholder content */
    lv_obj_t      *arrow[2];
    lv_obj_t      *arrow_lbl[2];
    lv_obj_t      *title, *count;
    ui_photo_img_t buf[2];
    int            cur;     /* buf in use */
    /* The photo shown (name "" = none): its name, mtime and size, so a
     * library change that leaves it as it was doesn't decode it again. */
    char           shown_name[PHOTO_NAME_MAX];
    time_t         shown_mtime;
    int64_t        shown_size;
    bool           failed;
} g_view;

static struct {
    lv_obj_t *obj;
} g_confirm;

static struct {
    lv_obj_t      *obj;
    lv_obj_t      *live;
    lv_obj_t      *pic;
    lv_obj_t      *img;
    lv_obj_t      *label;
    lv_obj_t      *empty;
    ui_photo_img_t im;
    char           name[PHOTO_NAME_MAX];
    time_t         mtime;
    int            w;
} g_tile;
static void (*g_tile_listener)(void);

static photo_lib_t  *g_lib;
static photos_view_t g_mode = PHOTOS_VIEW_EMPTY;
static int           g_sel = -1;
static char          g_sel_name[PHOTO_NAME_MAX];
static bool          g_confirm_open;
static bool          g_visible;
static unsigned      g_gen;               /* bumped on every reload */
static thumb_t       g_thumbs[THUMB_CACHE];
static uint32_t      g_lru;
static lv_timer_t   *g_load_timer;

#ifdef CHATBOX_TESTING
static int g_bounces, g_last_bounce, g_view_decodes;
#endif

static void render(void);

/* ---- Small helpers ------------------------------------------------------- */

static int count(void) { return photo_lib_count(g_lib); }

/* "14 November 2026" from a file time (local time). */
static void fmt_date(time_t t, char *out, size_t n)
{
    static const char *months[] = { "January", "February", "March", "April", "May", "June",
                                    "July", "August", "September", "October", "November",
                                    "December" };
    struct tm tm;
    if (!localtime_r(&t, &tm)) { snprintf(out, n, "%s", ""); return; }
    snprintf(out, n, "%d %s %d", tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

/* ---- Bounce -------------------------------------------------------------- */

_Static_assert(PHOTOS_BOUNCE_UP == (int)UI_BOUNCE_UP && PHOTOS_BOUNCE_DOWN == (int)UI_BOUNCE_DOWN &&
               PHOTOS_BOUNCE_LEFT == (int)UI_BOUNCE_LEFT && PHOTOS_BOUNCE_RIGHT == (int)UI_BOUNCE_RIGHT,
               "PHOTOS_BOUNCE_* are ui_bounce_t values");

static void bounce(lv_obj_t *obj, int kind)
{
#ifdef CHATBOX_TESTING
    g_bounces++;
    g_last_bounce = kind;
#endif
    ui_bounce(obj, (ui_bounce_t)kind);
}

/* ---- Thumbnails ---------------------------------------------------------- */

static thumb_t *thumb_find(const photo_t *p)
{
    for (int i = 0; i < THUMB_CACHE; i++) {
        thumb_t *t = &g_thumbs[i];
        if (t->used && t->mtime == p->mtime && t->size == p->size && strcmp(t->name, p->name) == 0) {
            t->lru = ++g_lru;
            return t;
        }
    }
    return NULL;
}

static bool thumb_in_use(const thumb_t *t)
{
    for (int j = 0; j < g_grid.pool; j++)
        if (g_grid.cards[j].th == t) return true;
    return false;
}

/* A free slot, else the least recently used one no card shows. */
static thumb_t *thumb_alloc(void)
{
    thumb_t *best = NULL;
    for (int i = 0; i < THUMB_CACHE; i++) {
        thumb_t *t = &g_thumbs[i];
        if (!t->used) return t;
        if (thumb_in_use(t)) continue;
        if (!best || t->lru < best->lru) best = t;
    }
    if (best) {
        ui_photo_img_free(&best->img);
        memset(best, 0, sizeof(*best));
    }
    return best;
}

static void thumbs_clear(void)
{
    for (int i = 0; i < THUMB_CACHE; i++) {
        ui_photo_img_free(&g_thumbs[i].img);
        memset(&g_thumbs[i], 0, sizeof(g_thumbs[i]));
    }
}

/* ---- Cards --------------------------------------------------------------- */

static void card_create(card_t *c, lv_obj_t *parent)
{
    memset(c, 0, sizeof(*c));
    c->item = -1;
    c->obj = ui_obj(parent);
    lv_obj_set_size(c->obj, CARD_W, g_grid.card_h);
    lv_obj_set_style_radius(c->obj, UI_RADIUS, 0);

    c->pic = ui_obj(c->obj);
    lv_obj_set_size(c->pic, PIC_W, PIC_H);
    lv_obj_set_pos(c->pic, CARD_PAD, CARD_PAD);
    lv_obj_set_style_radius(c->pic, PIC_RADIUS, 0);
    lv_obj_set_style_clip_corner(c->pic, true, 0);
    lv_obj_set_style_bg_color(c->pic, UI_C_PHOTOS_SOFT, 0);
    lv_obj_set_style_bg_opa(c->pic, LV_OPA_COVER, 0);

    c->icon = ui_label(c->pic, &lv_font_montserrat_48, UI_C_PHOTOS_INK);
    lv_label_set_text(c->icon, LV_SYMBOL_IMAGE);
    lv_obj_center(c->icon);
    lv_obj_add_flag(c->icon, LV_OBJ_FLAG_HIDDEN);

    c->img = lv_image_create(c->pic);
    lv_obj_set_pos(c->img, 0, 0);
    lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);

    c->cap = ui_label(c->obj, &font_chat_28, UI_C_INK);
    ui_label_one_line(c->cap, PIC_W);
    lv_obj_set_pos(c->cap, CARD_PAD, CARD_PAD + PIC_H + 8);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_HIDDEN);
}

/* Show thumbnail `t` (NULL: loading) in card c. */
static void card_show(card_t *c, thumb_t *t)
{
    c->th = t;
    if (t && !t->failed) {
        ui_photo_img_unset(c->img);
        lv_image_set_src(c->img, &t->img.dsc);
        ui_set_hidden(c->img, false);
        ui_set_hidden(c->icon, true);
        lv_obj_set_style_bg_opa(c->pic, LV_OPA_TRANSP, 0);   /* the picture covers it */
    } else {
        ui_photo_img_unset(c->img);
        ui_set_hidden(c->img, true);
        ui_set_hidden(c->icon, t == NULL);                     /* failed: the glyph */
        lv_obj_set_style_bg_opa(c->pic, LV_OPA_COVER, 0);
    }
}

static void load_timer_update(void);

static void card_bind(card_t *c, int i)
{
    const photo_t *p = photo_lib_get(g_lib, i);
    if (!p) return;
    if (c->item == i && c->gen == g_gen) return;
    c->item = i;
    c->gen = g_gen;
    char date[48];
    fmt_date(p->mtime, date, sizeof(date));
    set_text(c->cap, date);
    thumb_t *t = thumb_find(p);
    card_show(c, t);
    c->pending = t == NULL;
}

static void card_unbind(card_t *c)
{
    ui_set_hidden(c->obj, true);
    c->item = -1;
    c->pending = false;
    card_show(c, NULL);
    ui_set_hidden(c->icon, true);
}

/* Decode the thumbnail of card c now. */
static void card_load(card_t *c)
{
    c->pending = false;
    const photo_t *p = photo_lib_get(g_lib, c->item);
    char path[1024];
    if (!p || !photo_lib_path(g_lib, c->item, path, sizeof(path))) return;
    thumb_t *t = thumb_alloc();
    if (!t) return;
    t->used = true;
    snprintf(t->name, sizeof(t->name), "%s", p->name);
    t->mtime = p->mtime;
    t->size = p->size;
    t->lru = ++g_lru;
    t->failed = !ui_photo_img_load(&t->img, path, PIC_W, PIC_H, PHOTO_FIT_COVER);
    card_show(c, t);
}

/* The waiting card nearest the selection, or NULL. */
static card_t *next_pending(void)
{
    card_t *best = NULL;
    int bd = 0;
    for (int j = 0; j < g_grid.pool; j++) {
        card_t *c = &g_grid.cards[j];
        if (!c->pending || c->item < 0) continue;
        int d = c->item - (g_sel < 0 ? 0 : g_sel);
        if (d < 0) d = -d;
        if (!best || d < bd) { best = c; bd = d; }
    }
    return best;
}

static void load_timer_cb(lv_timer_t *t)
{
    (void)t;
    card_t *c = (g_mode == PHOTOS_VIEW_GRID) ? next_pending() : NULL;
    if (c) card_load(c);
    else load_timer_update();
}

static void load_timer_update(void)
{
    if (!g_load_timer) return;
    bool run = g_visible && g_mode == PHOTOS_VIEW_GRID && next_pending() != NULL;
    if (run) lv_timer_resume(g_load_timer);
    else     lv_timer_pause(g_load_timer);
}

/* ---- Grid ---------------------------------------------------------------- */

static void render_grid(void)
{
    int n = count(), sel = g_sel;
    int pitch = g_grid.card_h + CARD_GAP_Y;
    int rows = (n + GRID_COLS - 1) / GRID_COLS;
    int max = rows * pitch - CARD_GAP_Y + 2 * GRID_PAD_Y - VIEW_H;
    if (max < 0) max = 0;
    int y = sel >= 0 ? (sel / GRID_COLS) * pitch + GRID_PAD_Y - (VIEW_H - g_grid.card_h) / 2 : 0;
    if (y > max) y = max;
    if (y < 0) y = 0;

    int first = (y - GRID_PAD_Y) / pitch;
    if (first < 0) first = 0;
    for (int j = 0; j < g_grid.pool; j++) {
        card_t *c = &g_grid.cards[j];
        int item = first * GRID_COLS + j;
        if (item >= n) {
            card_unbind(c);
            continue;
        }
        card_bind(c, item);
        lv_obj_set_pos(c->obj, GRID_X + (item % GRID_COLS) * (CARD_W + CARD_GAP_X),
                       GRID_PAD_Y + (item / GRID_COLS) * pitch - y);
        ui_set_hidden(c->obj, false);
        bool focused = item == sel && !g_confirm_open;
        bool chosen  = item == sel && g_confirm_open;
        ui_item_style(c->obj, focused ? UI_ITEM_FOCUSED : chosen ? UI_ITEM_SELECTED : UI_ITEM_IDLE,
                      UI_APP_PHOTOS);
    }
    load_timer_update();
}

/* ---- Viewer -------------------------------------------------------------- */

static void viewer_arrow(int side, bool dim)
{
    lv_obj_set_style_bg_color(g_view.arrow[side], dim ? UI_C_SUNK : UI_C_SURFACE, 0);
    lv_obj_set_style_text_color(g_view.arrow_lbl[side], dim ? UI_C_INK_2 : UI_C_INK, 0);
    lv_obj_set_style_outline_color(g_view.arrow[side], dim ? UI_C_LINE : UI_C_INK, 0);
}

/* Put photo g_sel in the viewer (decoding it unless it already shows). */
static void render_viewer(void)
{
    const photo_t *p = photo_lib_get(g_lib, g_sel);
    if (!p) return;
    if (strcmp(g_view.shown_name, p->name) != 0 || g_view.shown_mtime != p->mtime ||
        g_view.shown_size != p->size) {
        char path[1024];
        int nb = 1 - g_view.cur;
#ifdef CHATBOX_TESTING
        g_view_decodes++;
#endif
        bool ok = photo_lib_path(g_lib, g_sel, path, sizeof(path)) &&
                  ui_photo_img_load(&g_view.buf[nb], path, BIG_W, BIG_H, PHOTO_FIT_CONTAIN);
        int w, h;
        ui_photo_img_unset(g_view.img);
        if (ok) {
            w = g_view.buf[nb].px.w;
            h = g_view.buf[nb].px.h;
            lv_image_set_src(g_view.img, &g_view.buf[nb].dsc);
            ui_set_hidden(g_view.img, false);
            ui_set_hidden(g_view.fail, true);
            lv_obj_set_style_bg_opa(g_view.frame, LV_OPA_TRANSP, 0);
        } else {
            w = BIG_H * 4 / 3;
            h = BIG_H;
            ui_set_hidden(g_view.img, true);
            ui_set_hidden(g_view.fail, false);
            lv_obj_set_style_bg_opa(g_view.frame, LV_OPA_COVER, 0);
        }
        ui_photo_img_free(&g_view.buf[g_view.cur]);
        g_view.cur = nb;
        g_view.failed = !ok;
        snprintf(g_view.shown_name, sizeof(g_view.shown_name), "%s", p->name);
        g_view.shown_mtime = p->mtime;
        g_view.shown_size = p->size;
        lv_obj_set_size(g_view.frame, w, h);
        lv_obj_set_pos(g_view.frame, (SCR_W - w) / 2, AREA_Y + (AREA_H - h) / 2);
    }
    char date[48], cnt[32];
    fmt_date(p->mtime, date, sizeof(date));
    set_text(g_view.title, date);
    snprintf(cnt, sizeof(cnt), "%d of %d", g_sel + 1, count());
    set_text(g_view.count, cnt);
    viewer_arrow(0, g_sel <= 0);
    viewer_arrow(1, g_sel >= count() - 1);
}

static void viewer_forget(void)
{
    if (g_view.img) ui_photo_img_unset(g_view.img);
    ui_photo_img_free(&g_view.buf[0]);
    ui_photo_img_free(&g_view.buf[1]);
    g_view.shown_name[0] = '\0';
}

/* ---- Views --------------------------------------------------------------- */

static void select_index(int i)
{
    int n = count();
    if (n == 0) { g_sel = -1; g_sel_name[0] = '\0'; return; }
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    g_sel = i;
    snprintf(g_sel_name, sizeof(g_sel_name), "%s", photo_lib_get(g_lib, i)->name);
}

static void render(void)
{
    if (!g_root) return;
    int n = count();
    if (n == 0) g_mode = PHOTOS_VIEW_EMPTY;
    else if (g_mode == PHOTOS_VIEW_EMPTY) g_mode = PHOTOS_VIEW_GRID;
    if (g_mode == PHOTOS_VIEW_EMPTY) g_confirm_open = false;

    ui_set_hidden(g_empty, g_mode != PHOTOS_VIEW_EMPTY);
    ui_set_hidden(g_grid.view, g_mode != PHOTOS_VIEW_GRID);
    ui_set_hidden(g_view.obj, g_mode != PHOTOS_VIEW_VIEWER);
    ui_set_hidden(g_head_sub, g_mode != PHOTOS_VIEW_GRID);
    ui_set_hidden(g_confirm.obj, !g_confirm_open);
    if (g_confirm_open) lv_obj_move_foreground(g_confirm.obj);

    char sub[48];
    snprintf(sub, sizeof(sub), n == 1 ? "1 photo" : "%d photos", n);
    set_text(g_head_sub, sub);

    if (g_mode == PHOTOS_VIEW_GRID) render_grid();
    else if (g_mode == PHOTOS_VIEW_VIEWER) render_viewer();
    if (g_mode != PHOTOS_VIEW_VIEWER) viewer_forget();
    load_timer_update();
}

photos_view_t ui_photos_view(void) { return g_mode; }
bool ui_photos_confirm_open(void)  { return g_confirm_open; }

key_ctx_t ui_photos_key_ctx(void) { return KEY_CTX_PHOTOS; }

void ui_photos_reload(void)
{
    if (!g_root) return;
    g_gen++;
    for (int j = 0; j < g_grid.pool; j++) g_grid.cards[j].item = -1;
    /* Keep the selection on the same photo when it's still there. When it
     * is gone, a neighbour takes the index, but the delete question was
     * about the photo that went: it closes, so Enter can't delete a photo
     * nobody chose. */
    int i = g_sel_name[0] ? photo_lib_find(g_lib, g_sel_name) : -1;
    if (i < 0) g_confirm_open = false;
    select_index(i >= 0 ? i : (g_sel < 0 ? 0 : g_sel));
    render();           /* the viewer decodes again only if its photo changed */
    if (g_tile_listener) g_tile_listener();
}

void ui_photos_set_library(photo_lib_t *lib)
{
    g_lib = lib;
    thumbs_clear();
    g_sel = -1;
    g_sel_name[0] = '\0';
    ui_photos_reload();
}

/* ---- Keys ---------------------------------------------------------------- */

static void open_confirm(void)
{
    if (g_sel < 0) return;
    g_confirm_open = true;
    render();
}

static void close_confirm(void)
{
    g_confirm_open = false;
    render();
}

static void do_delete(void)
{
    g_confirm_open = false;
    /* photo_lib_delete() rescans whether or not the unlink worked (a
     * read-only folder keeps the photo listed; a file already gone just
     * drops off), so redraw from the library as it is now, like any change:
     * the deleted photo isn't found, so the one that slid into its index is
     * selected (past the end: the new last; none left: "No photos yet"). */
    bool ok = photo_lib_delete(g_lib, g_sel) == 0;
    ui_photos_reload();
    if (!ok) bounce(g_mode == PHOTOS_VIEW_VIEWER ? g_view.frame : g_grid.list, PHOTOS_BOUNCE_RIGHT);
}

static void key_grid(uint32_t key)
{
    int n = count(), sel = g_sel;
    int ni = -1, kind = PHOTOS_BOUNCE_NONE;
    switch (key) {
    case LV_KEY_LEFT:
        if (sel % GRID_COLS == 0) kind = PHOTOS_BOUNCE_LEFT; else ni = sel - 1;
        break;
    case LV_KEY_RIGHT:
        if (sel % GRID_COLS == GRID_COLS - 1 || sel + 1 >= n) kind = PHOTOS_BOUNCE_RIGHT;
        else ni = sel + 1;
        break;
    case LV_KEY_UP:
        if (sel - GRID_COLS < 0) kind = PHOTOS_BOUNCE_UP; else ni = sel - GRID_COLS;
        break;
    case LV_KEY_DOWN:
        if (sel + GRID_COLS < n) ni = sel + GRID_COLS;
        else if (sel / GRID_COLS < (n - 1) / GRID_COLS) ni = n - 1;
        else kind = PHOTOS_BOUNCE_DOWN;
        break;
    case LV_KEY_ENTER:
        g_mode = PHOTOS_VIEW_VIEWER;
        render();
        return;
    case LV_KEY_DEL:
        open_confirm();
        return;
    default:
        return;     /* Tab, letters, Space, Win, PgUp/PgDn: ignored */
    }
    if (kind != PHOTOS_BOUNCE_NONE) {
        bounce(g_grid.list, kind);
        return;
    }
    select_index(ni);
    render_grid();
}

static void key_viewer(uint32_t key)
{
    switch (key) {
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT: {
        int d = key == LV_KEY_LEFT ? -1 : 1;
        int ni = g_sel + d;
        if (ni < 0 || ni >= count()) {
            bounce(g_view.frame, d < 0 ? PHOTOS_BOUNCE_LEFT : PHOTOS_BOUNCE_RIGHT);
            return;
        }
        select_index(ni);
        render_viewer();
        return;
    }
    case LV_KEY_DEL:
        open_confirm();
        return;
    default:
        return;     /* ↑ ↓ Enter, letters...: ignored */
    }
}

void ui_photos_handle_key(uint32_t key)
{
    if (!g_root) return;
    if (g_confirm_open) {
        if (key == LV_KEY_ENTER) do_delete();
        return;     /* everything else waits for Enter or Esc */
    }
    if (g_mode == PHOTOS_VIEW_GRID) key_grid(key);
    else if (g_mode == PHOTOS_VIEW_VIEWER) key_viewer(key);
    /* EMPTY: nothing to do */
}

bool ui_photos_handle_esc(void)
{
    if (!g_root) return false;
    if (g_confirm_open) {
        close_confirm();
        return true;
    }
    if (g_mode == PHOTOS_VIEW_VIEWER) {
        g_mode = PHOTOS_VIEW_GRID;
        render();
        return true;
    }
    return false;
}

/* ---- Hints --------------------------------------------------------------- */

static const ui_hint_t k_hints_grid[] = {
    { { UI_KEY_LEFT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_RIGHT }, "choose", false },
    { { "Enter" }, "open", false },
    { { "Del" }, "delete", false },
    { { "Esc" }, "home", true },
};
static const ui_hint_t k_hints_viewer[] = {
    { { UI_KEY_LEFT, UI_KEY_RIGHT }, "photos", false },
    { { "Del" }, "delete", false },
    { { "Esc" }, "all photos", true },
};
static const ui_hint_t k_hints_confirm[] = {
    { { "Enter" }, "delete", false },
    { { "Esc" }, "keep", true },
};
static const ui_hint_t k_hints_empty[] = {
    { { "Esc" }, "home", true },
};
#define N_HINTS(a) ((int)(sizeof(a) / sizeof((a)[0])))

const ui_hint_t *ui_photos_hints(int *n)
{
    if (g_confirm_open) { *n = N_HINTS(k_hints_confirm); return k_hints_confirm; }
    switch (g_mode) {
    case PHOTOS_VIEW_GRID:   *n = N_HINTS(k_hints_grid);   return k_hints_grid;
    case PHOTOS_VIEW_VIEWER: *n = N_HINTS(k_hints_viewer); return k_hints_viewer;
    default:                 *n = N_HINTS(k_hints_empty);  return k_hints_empty;
    }
}

/* ---- Building ------------------------------------------------------------ */

static void head_create(lv_obj_t *parent)
{
    g_head = ui_obj(parent);
    lv_obj_set_size(g_head, SCR_W - 2 * HEAD_X, HEAD_H);
    lv_obj_set_pos(g_head, HEAD_X, 0);
    lv_obj_set_flex_flow(g_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_head, 12, 0);
    lv_obj_t *icon = ui_label(g_head, &lv_font_montserrat_32, UI_C_PHOTOS_INK);
    lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    lv_obj_t *t = ui_label(g_head, &font_title_36, UI_C_PHOTOS_INK);
    lv_label_set_text(t, "Photos");
    g_head_sub = ui_label(g_head, &font_chat_28, UI_C_INK_2);
    lv_obj_set_style_pad_left(g_head_sub, 4, 0);
}

static void grid_create(lv_obj_t *parent)
{
    int lh = lv_font_get_line_height(&font_chat_28);
    g_grid.card_h = CARD_PAD + PIC_H + 8 + lh + CARD_PAD;
    g_grid.view = ui_obj(parent);
    lv_obj_set_pos(g_grid.view, 0, HEAD_H);
    lv_obj_set_size(g_grid.view, SCR_W, VIEW_H);
    g_grid.list = ui_obj(g_grid.view);
    lv_obj_set_size(g_grid.list, SCR_W, VIEW_H);
    lv_obj_add_flag(g_grid.list, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int pitch = g_grid.card_h + CARD_GAP_Y;
    g_grid.pool = ((VIEW_H + pitch - 1) / pitch + 1) * GRID_COLS;
    if (g_grid.pool > CARD_POOL) g_grid.pool = CARD_POOL;
    for (int j = 0; j < g_grid.pool; j++) card_create(&g_grid.cards[j], g_grid.list);
}

static lv_obj_t *arrow_create(lv_obj_t *parent, int side)
{
    lv_obj_t *a = ui_obj(parent);
    lv_obj_set_size(a, ARROW, ARROW);
    lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(a, UI_C_SURFACE, 0);
    lv_obj_set_style_outline_width(a, 3, 0);
    lv_obj_set_style_outline_pad(a, 0, 0);
    lv_obj_set_style_outline_color(a, UI_C_INK, 0);
    lv_obj_set_style_outline_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_pos(a, side == 0 ? ARROW_X : SCR_W - ARROW_X - ARROW, AREA_Y + (AREA_H - ARROW) / 2);
    lv_obj_t *l = ui_label(a, &lv_font_montserrat_36, UI_C_INK);
    lv_label_set_text(l, side == 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    lv_obj_center(l);
    g_view.arrow_lbl[side] = l;
    return a;
}

static void viewer_create(lv_obj_t *parent)
{
    g_view.shown_name[0] = '\0';
    g_view.obj = ui_obj(parent);
    lv_obj_set_pos(g_view.obj, 0, 0);
    lv_obj_set_size(g_view.obj, SCR_W, SCR_H);

    g_view.frame = ui_obj(g_view.obj);
    lv_obj_set_style_radius(g_view.frame, BIG_RADIUS, 0);
    lv_obj_set_style_clip_corner(g_view.frame, true, 0);
    lv_obj_set_style_bg_color(g_view.frame, UI_C_PHOTOS_SOFT, 0);
    lv_obj_set_style_bg_opa(g_view.frame, LV_OPA_COVER, 0);
    ui_sticker(g_view.frame, BIG_RING, BIG_SHADOW);

    g_view.img = lv_image_create(g_view.frame);
    lv_obj_set_pos(g_view.img, 0, 0);
    lv_obj_add_flag(g_view.img, LV_OBJ_FLAG_HIDDEN);

    g_view.fail = ui_obj(g_view.frame);
    lv_obj_set_size(g_view.fail, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(g_view.fail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_view.fail, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_view.fail, 16, 0);
    lv_obj_t *gl = ui_label(g_view.fail, &font_icon_120, UI_C_PHOTOS_INK);
    lv_label_set_text(gl, LV_SYMBOL_IMAGE);
    lv_obj_t *msg = ui_label(g_view.fail, &font_chat_36, UI_C_PHOTOS_INK);
    lv_label_set_text(msg, "This photo can't be shown");
    lv_obj_add_flag(g_view.fail, LV_OBJ_FLAG_HIDDEN);

    g_view.arrow[0] = arrow_create(g_view.obj, 0);
    g_view.arrow[1] = arrow_create(g_view.obj, 1);

    lv_obj_t *bar = ui_obj(g_view.obj);
    lv_obj_set_size(bar, SCR_W - 2 * 56, BAR_H);
    lv_obj_set_pos(bar, 56, SCR_H - BAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    g_view.title = ui_label(bar, &font_title_36, UI_C_INK);
    ui_label_one_line(g_view.title, SCR_W - 2 * 56);
    g_view.count = ui_label(bar, &font_chat_28, UI_C_INK_2);
    ui_label_one_line(g_view.count, SCR_W - 2 * 56);
}

static void empty_create(lv_obj_t *parent)
{
    lv_obj_t *n = ui_obj(parent);
    g_empty = n;
    lv_obj_set_size(n, SCR_W, SCR_H - HEAD_H);
    lv_obj_set_pos(n, 0, HEAD_H);

    /* The picture on a round green sticker (like "No music yet"). */
    lv_obj_t *badge = ui_obj(n);
    lv_obj_set_size(badge, 200, 200);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, -144);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge, UI_C_PHOTOS, 0);
    ui_sticker(badge, 4, 10);
    lv_obj_t *gl = ui_label(badge, &font_icon_120, UI_C_ON_PHOTOS);
    lv_label_set_text(gl, LV_SYMBOL_IMAGE);
    lv_obj_center(gl);

    lv_obj_t *msg = ui_label(n, &font_title_48, UI_C_INK);
    lv_label_set_text(msg, "No photos yet");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 36);

    lv_obj_t *sub = ui_label(n, &font_chat_28, UI_C_INK_2);
    lv_label_set_text(sub, "Photos sent to you in chat show up here.");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 106);
}

/* A keycap and a word, on a pill: "[Enter] Delete". */
static void choice_create(lv_obj_t *parent, const char *key, const char *word, bool alert)
{
    lv_obj_t *b = ui_obj(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, alert ? UI_C_ALERT : UI_C_SURFACE, 0);
    ui_sticker(b, 3, 5);
    lv_obj_set_style_pad_left(b, 12, 0);
    lv_obj_set_style_pad_right(b, 26, 0);
    lv_obj_set_style_pad_column(b, 14, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = ui_obj(b);
    lv_obj_set_size(cap, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_radius(cap, 12, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cap, UI_C_SURFACE, 0);
    lv_obj_set_style_border_width(cap, 2, 0);
    lv_obj_set_style_border_color(cap, UI_C_INK, 0);
    lv_obj_set_style_pad_hor(cap, 12, 0);
    lv_obj_t *k = ui_label(cap, &font_title_28, UI_C_INK);
    lv_label_set_text(k, key);
    lv_obj_center(k);

    lv_obj_t *w = ui_label(b, &font_title_36, alert ? UI_C_ON_ALERT : UI_C_INK);
    lv_label_set_text(w, word);
}

static void confirm_create(lv_obj_t *parent)
{
    lv_obj_t *c = ui_obj(parent);
    g_confirm.obj = c;
    lv_obj_set_size(c, 640, LV_SIZE_CONTENT);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(c, UI_RADIUS_BIG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, UI_C_SURFACE, 0);
    ui_sticker(c, 5, 14);
    lv_obj_set_style_pad_all(c, 40, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *t = ui_label(c, &font_title_48, UI_C_INK);
    lv_label_set_text(t, "Delete this photo?");
    lv_obj_t *s = ui_label(c, &font_chat_28, UI_C_INK_2);
    lv_label_set_text(s, "It will be gone from Photos.");

    lv_obj_t *row = ui_obj(c);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 20, 0);
    lv_obj_set_style_pad_bottom(row, 6, 0);
    lv_obj_set_style_pad_column(row, 28, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    choice_create(row, "Enter", "Delete", true);
    choice_create(row, "Esc", "Keep", false);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Live tile (T9) ------------------------------------------------------ */

static void tile_delete_cb(lv_event_t *e)
{
    (void)e;
    ui_photo_img_free(&g_tile.im);
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
    int pw = g_tile.w - 2 * TILE_INSET, ph = pw * 3 / 4;
    g_tile.pic = ui_obj(g_tile.live);
    lv_obj_set_size(g_tile.pic, pw, ph);
    lv_obj_set_pos(g_tile.pic, TILE_INSET, TILE_INSET);
    lv_obj_set_style_radius(g_tile.pic, 22, 0);
    lv_obj_set_style_clip_corner(g_tile.pic, true, 0);
    lv_obj_set_style_bg_color(g_tile.pic, UI_C_PHOTOS_SOFT, 0);
    lv_obj_set_style_bg_opa(g_tile.pic, LV_OPA_COVER, 0);
    ui_sticker(g_tile.pic, 4, 0);
    g_tile.img = lv_image_create(g_tile.pic);
    lv_obj_set_pos(g_tile.img, 0, 0);
    g_tile.label = ui_label(g_tile.live, &font_title_36, UI_C_ON_PHOTOS);
    lv_obj_set_pos(g_tile.label, TILE_INSET, TILE_INSET + ph + 16);

    g_tile.empty = ui_label(g_tile.obj, &font_icon_120, UI_C_ON_PHOTOS);
    lv_label_set_text(g_tile.empty, LV_SYMBOL_IMAGE);
    lv_obj_center(g_tile.empty);
}

void ui_photos_tile_render(lv_obj_t *face)
{
    if (!face) return;
    if (!g_tile.obj || lv_obj_get_parent(g_tile.obj) != face) {
        if (g_tile.obj) lv_obj_delete(g_tile.obj);
        tile_build(face);
    }
    int n = count();
    ui_set_hidden(g_tile.live, n == 0);
    ui_set_hidden(g_tile.empty, n != 0);
    if (n == 0) return;

    const photo_t *p = photo_lib_get(g_lib, 0);
    if (strcmp(g_tile.name, p->name) != 0 || g_tile.mtime != p->mtime) {
        char path[1024];
        ui_photo_img_unset(g_tile.img);
        bool ok = photo_lib_path(g_lib, 0, path, sizeof(path)) &&
                  ui_photo_img_load(&g_tile.im, path, lv_obj_get_style_width(g_tile.pic, 0),
                                    lv_obj_get_style_height(g_tile.pic, 0), PHOTO_FIT_COVER);
        if (ok) lv_image_set_src(g_tile.img, &g_tile.im.dsc);
        ui_set_hidden(g_tile.img, !ok);
        snprintf(g_tile.name, sizeof(g_tile.name), "%s", p->name);
        g_tile.mtime = p->mtime;
    }
    char t[48];
    snprintf(t, sizeof(t), n == 1 ? "1 photo" : "%d photos", n);
    set_text(g_tile.label, t);
}

void ui_photos_set_tile_listener(void (*cb)(void))
{
    g_tile_listener = cb;
}

/* ---- Lifecycle ----------------------------------------------------------- */

static void root_delete_cb(lv_event_t *e)
{
    (void)e;
    if (g_load_timer) lv_timer_delete(g_load_timer);
    g_load_timer = NULL;
    ui_photo_img_free(&g_view.buf[0]);
    ui_photo_img_free(&g_view.buf[1]);
    thumbs_clear();
    g_root = NULL;
    g_head = g_head_sub = g_empty = NULL;
    memset(&g_grid, 0, sizeof(g_grid));
    memset(&g_view, 0, sizeof(g_view));
    memset(&g_confirm, 0, sizeof(g_confirm));
    g_visible = false;
}

void ui_photos_create(lv_obj_t *root)
{
    g_root = root;
    g_mode = PHOTOS_VIEW_EMPTY;
    g_sel = -1;
    g_sel_name[0] = '\0';
    g_confirm_open = false;
    g_visible = false;
#ifdef CHATBOX_TESTING
    g_bounces = 0;
    g_last_bounce = PHOTOS_BOUNCE_NONE;
#endif
    lv_obj_set_style_bg_color(root, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);

    grid_create(root);
    empty_create(root);
    viewer_create(root);
    head_create(root);          /* above the viewer: it shares the header */
    confirm_create(root);

    g_load_timer = lv_timer_create(load_timer_cb, LOAD_MS, NULL);
    lv_timer_pause(g_load_timer);
    ui_photos_reload();
}

void ui_photos_on_show(void)
{
    g_visible = true;
    load_timer_update();
}

void ui_photos_on_hide(void)
{
    g_visible = false;
    load_timer_update();
}

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

void ui_photos_test_flush(void)
{
    card_t *c;
    while ((c = next_pending()) != NULL) card_load(c);
    load_timer_update();
}

void ui_photos_test_state(photos_test_state_t *o)
{
    memset(o, 0, sizeof(*o));
    o->view = g_mode;
    o->count = count();
    o->sel = g_sel;
    o->confirm_open = g_confirm_open && !is_hidden(g_confirm.obj);
    copy_label(o->head_sub, sizeof(o->head_sub), g_head_sub);
    o->empty_visible = !is_hidden(g_empty);
    o->grid_visible = !is_hidden(g_grid.view);
    o->viewer_visible = !is_hidden(g_view.obj);
    for (int j = 0; j < g_grid.pool; j++) {
        card_t *c = &g_grid.cards[j];
        if (c->item < 0 || is_hidden(c->obj)) continue;
        o->cards_bound++;
        if (c->th && !c->th->failed && !is_hidden(c->img)) o->cards_image++;
        else if (c->th && c->th->failed && !is_hidden(c->icon)) o->cards_failed++;
        else if (!c->th) o->cards_loading++;
        if (c->item == g_sel) {
            o->sel_card_focused = lv_obj_get_style_outline_width(c->obj, 0) > 0;
            copy_label(o->sel_caption, sizeof(o->sel_caption), c->cap);
        }
    }
    copy_label(o->viewer_title, sizeof(o->viewer_title), g_view.title);
    copy_label(o->viewer_count, sizeof(o->viewer_count), g_view.count);
    o->viewer_image = !is_hidden(g_view.img) && !g_view.failed;
    o->viewer_failed = g_view.failed && !is_hidden(g_view.fail);
    o->viewer_img_w = g_view.frame ? lv_obj_get_style_width(g_view.frame, 0) : 0;
    o->viewer_img_h = g_view.frame ? lv_obj_get_style_height(g_view.frame, 0) : 0;
    o->viewer_decodes = g_view_decodes;
    if (g_view.arrow[0]) {
        o->arrow_left_dim  = lv_color_eq(lv_obj_get_style_bg_color(g_view.arrow[0], 0), UI_C_SUNK);
        o->arrow_right_dim = lv_color_eq(lv_obj_get_style_bg_color(g_view.arrow[1], 0), UI_C_SUNK);
    }
    o->bounces = g_bounces;
    o->last_bounce = g_last_bounce;
    o->tile_built = g_tile.obj != NULL;
    o->tile_image = g_tile.obj && !is_hidden(g_tile.live) && !is_hidden(g_tile.img) &&
                    ui_photo_img_ok(&g_tile.im);
    o->tile_empty = g_tile.obj && !is_hidden(g_tile.empty);
    copy_label(o->tile_label, sizeof(o->tile_label), g_tile.label);
}
#endif
