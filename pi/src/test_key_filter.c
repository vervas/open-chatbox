/**
 * test_key_filter.c — Unit tests for the one-shot key filter (spec D2, §8.1).
 *
 * Run: ./test_key_filter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "key_filter.h"

static int g_tests = 0;
static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* A few keys the §8.1 table never names. */
#define KEY_A     ((uint32_t)'a')
#define KEY_TAB   9u     /* LV_KEY_NEXT */
#define KEY_BKSP  8u     /* LV_KEY_BACKSPACE */

static const char *ctx_name(key_ctx_t c)
{
    switch (c) {
    case KEY_CTX_MUSIC_BROWSE: return "music_columns";
    case KEY_CTX_NOW_PLAYING:   return "now_playing";
    case KEY_CTX_NO_MUSIC:      return "no_music";
    case KEY_CTX_SWITCHER:      return "switcher";
    case KEY_CTX_CHAT:          return "chat";
    case KEY_CTX_PHOTOS:        return "photos";
    default:                    return "?";
    }
}

/* ---------------------------------------------------------------------- */
/* §8.1 one-shot table, every context × every key it names plus extras     */
/* ---------------------------------------------------------------------- */

static void test_one_shot_table(void)
{
    static const struct { uint32_t key; const char *name; } keys[] = {
        { KF_KEY_SPACE, "Space" }, { KF_KEY_ENTER, "Enter" },
        { KF_KEY_LEFT,  "Left"  }, { KF_KEY_RIGHT, "Right" },
        { KF_KEY_ESC,   "Esc"   }, { KF_KEY_UP,    "Up"    },
        { KF_KEY_DOWN,  "Down"  }, { KF_KEY_PGUP,  "PgUp"  },
        { KF_KEY_PGDN,  "PgDn"  }, { KF_KEY_MENU,  "Menu"  },
        { KEY_TAB,      "Tab"   }, { KEY_A,        "a"     },
        { KEY_BKSP,     "Bksp"  }, { KF_KEY_DEL,   "Del"   },
    };
    /* Rows: contexts; columns: keys above. 1 = one-shot. */
    static const int expect[KEY_CTX_COUNT][14] = {
        /*                   Spc Ent  L   R  Esc Up  Dn PgU PgD Mnu Tab  a  Bs Del */
        [KEY_CTX_MUSIC_BROWSE] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0},
        [KEY_CTX_NOW_PLAYING]   = {1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0},
        [KEY_CTX_NO_MUSIC]      = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        [KEY_CTX_SWITCHER]      = {0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        [KEY_CTX_CHAT]          = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        [KEY_CTX_PHOTOS]        = {0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    };

    for (int c = 0; c < KEY_CTX_COUNT; c++) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "one-shot(%s, %s) should be %d",
                     ctx_name((key_ctx_t)c), keys[k].name, expect[c][k]);
            EXPECT(key_filter_is_one_shot(keys[k].key, (key_ctx_t)c) ==
                   (expect[c][k] != 0), msg);
        }
    }
    EXPECT(!key_filter_is_one_shot(KF_KEY_ESC, KEY_CTX_COUNT),
           "unknown context is never one-shot");
}

/* ---------------------------------------------------------------------- */
/* Window boundary                                                         */
/* ---------------------------------------------------------------------- */

static void test_boundary(void)
{
    key_filter_t kf;

    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, 1000, true), "first press accepted");
    EXPECT(!key_filter_accept(&kf, KF_KEY_ENTER, 1449, true), "449 ms dropped");

    key_filter_init(&kf);
    key_filter_accept(&kf, KF_KEY_ENTER, 1000, true);
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, 1450, true), "450 ms accepted");

    key_filter_init(&kf);
    key_filter_accept(&kf, KF_KEY_ENTER, 1000, true);
    EXPECT(!key_filter_accept(&kf, KF_KEY_ENTER, 1000, true), "same-ms repeat dropped");

    /* Presses 500 ms apart are two deliberate presses. */
    key_filter_init(&kf);
    int n = 0;
    n += key_filter_accept(&kf, KF_KEY_SPACE, 0, true);
    n += key_filter_accept(&kf, KF_KEY_SPACE, 500, true);
    EXPECT(n == 2, "presses 500 ms apart -> 2 accepts");

    /* First-ever event at tick 0..449 is accepted (no previous record). */
    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_ESC, 10, true), "fresh key at low tick accepted");
}

/* ---------------------------------------------------------------------- */
/* Held keys                                                               */
/* ---------------------------------------------------------------------- */

/* Pi / LVGL cadence: press at t0, repeats at +400 then every 200 ms. */
static int hold_lvgl(key_filter_t *kf, uint32_t key, uint32_t t0,
                     int repeats, bool one_shot)
{
    int accepted = key_filter_accept(kf, key, t0, one_shot);
    for (int i = 0; i < repeats; i++)
        accepted += key_filter_accept(kf, key, t0 + 400 + 200u * (uint32_t)i,
                                      one_shot);
    return accepted;
}

static void test_hold_lvgl(void)
{
    key_filter_t kf;

    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_ESC, 5000, 20, true) == 1,
           "Pi hold of one-shot key -> exactly 1 accept");

    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_DOWN, 5000, 20, false) == 21,
           "Pi hold of repeat key -> every event accepted");

    /* Now Playing: ↑/↓ are volume (repeat when held), ←/→ skip (once). */
    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_UP, 5000, 10,
                     key_filter_is_one_shot(KF_KEY_UP, KEY_CTX_NOW_PLAYING)) == 11,
           "Now Playing: held Up (volume) -> every event accepted");
    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_DOWN, 5000, 10,
                     key_filter_is_one_shot(KF_KEY_DOWN, KEY_CTX_NOW_PLAYING)) == 11,
           "Now Playing: held Down (volume) -> every event accepted");
    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_RIGHT, 5000, 10,
                     key_filter_is_one_shot(KF_KEY_RIGHT, KEY_CTX_NOW_PLAYING)) == 1,
           "Now Playing: held Right (next track) -> exactly 1 accept");
    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_LEFT, 5000, 10,
                     key_filter_is_one_shot(KF_KEY_LEFT, KEY_CTX_NOW_PLAYING)) == 1,
           "Now Playing: held Left (previous track) -> exactly 1 accept");

    /* Per-context: hold every key, using the §8.1 table to choose. */
    for (int c = 0; c < KEY_CTX_COUNT; c++) {
        static const uint32_t keys[] = {
            KF_KEY_SPACE, KF_KEY_ENTER, KF_KEY_LEFT, KF_KEY_RIGHT, KF_KEY_ESC,
            KF_KEY_UP, KF_KEY_DOWN, KF_KEY_PGUP, KF_KEY_PGDN, KEY_A,
        };
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            bool os = key_filter_is_one_shot(keys[k], (key_ctx_t)c);
            key_filter_init(&kf);
            int got = hold_lvgl(&kf, keys[k], 100, 10, os);
            char msg[96];
            snprintf(msg, sizeof(msg), "hold key %u in %s -> %d accepts",
                     (unsigned)keys[k], ctx_name((key_ctx_t)c), os ? 1 : 11);
            EXPECT(got == (os ? 1 : 11), msg);
        }
    }
}

static void test_hold_sdl(void)
{
    /* SDL: press, first OS repeat at 450-ε, then every 33 ms. */
    key_filter_t kf;
    key_filter_init(&kf);
    int n = key_filter_accept(&kf, KF_KEY_ENTER, 0, true);
    n += key_filter_accept(&kf, KF_KEY_ENTER, 449, true);
    for (uint32_t t = 449 + 33; t < 5000; t += 33)
        n += key_filter_accept(&kf, KF_KEY_ENTER, t, true);
    EXPECT(n == 1, "SDL hold pattern -> exactly 1 accept");
}

/* Dropped events extend the window: each is recorded, so the window slides. */
static void test_sliding(void)
{
    key_filter_t kf;
    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 0, true), "t=0 accepted");
    EXPECT(!key_filter_accept(&kf, KF_KEY_LEFT, 300, true), "t=300 dropped");
    /* 600 is >= 450 after the first event but only 300 after the dropped one. */
    EXPECT(!key_filter_accept(&kf, KF_KEY_LEFT, 600, true),
           "t=600 dropped (window slid from dropped event)");
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 1050, true),
           "t=1050 accepted (450 after last event)");
}

/* Release, then press again: a real release (key_filter_release) resets
 * the record, so the next press is accepted however soon it comes. With no
 * release on record, the 450 ms window still applies. */
static void test_release_gap(void)
{
    key_filter_t kf;
    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_SPACE, 0, 5, true) == 1, "first hold fires once");
    /* Last repeat at 400 + 200*4 = 1200; release, press again at 1650. */
    key_filter_release(&kf, KF_KEY_SPACE);
    EXPECT(key_filter_accept(&kf, KF_KEY_SPACE, 1650, true),
           "press 450 ms after last repeat (released) accepted");
    key_filter_init(&kf);
    hold_lvgl(&kf, KF_KEY_SPACE, 0, 5, true);
    key_filter_release(&kf, KF_KEY_SPACE);
    EXPECT(key_filter_accept(&kf, KF_KEY_SPACE, 1250, true),
           "press 50 ms after a released hold accepted (release resets)");
    /* Formerly "double-tap counts once": without a release it still does. */
    key_filter_init(&kf);
    hold_lvgl(&kf, KF_KEY_SPACE, 0, 5, true);
    EXPECT(!key_filter_accept(&kf, KF_KEY_SPACE, 1600, true),
           "event 400 ms after last repeat with no release dropped");
    key_filter_init(&kf);
    hold_lvgl(&kf, KF_KEY_SPACE, 0, 5, true);
    EXPECT(key_filter_accept(&kf, KF_KEY_SPACE, 1650, true),
           "event 450 ms after last repeat with no release accepted");
}

/* Fast separate taps: every press acts when each is released. */
static void test_release_resets(void)
{
    key_filter_t kf;
    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_RIGHT, 0, true), "tap 1 accepted");
    key_filter_release(&kf, KF_KEY_RIGHT);
    EXPECT(key_filter_accept(&kf, KF_KEY_RIGHT, 50, true),
           "press, release, press 50 ms later -> accepted");
    key_filter_release(&kf, KF_KEY_RIGHT);
    EXPECT(key_filter_accept(&kf, KF_KEY_RIGHT, 51, true),
           "press 1 ms after a release accepted");

    /* No release in between: dropped (a repeat of a held key). */
    EXPECT(!key_filter_accept(&kf, KF_KEY_RIGHT, 100, true),
           "no release -> still dropped within 450 ms");
    EXPECT(!key_filter_accept(&kf, KF_KEY_RIGHT, 549, true),
           "no release -> window slides from the dropped event");

    /* A release of another key doesn't reset this one. */
    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, 0, true), "Enter accepted");
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 10, true), "Left accepted");
    key_filter_release(&kf, KF_KEY_LEFT);
    key_filter_release(&kf, KF_KEY_ESC);   /* no record at all: no-op */
    EXPECT(!key_filter_accept(&kf, KF_KEY_ENTER, 100, true),
           "release of another key doesn't reset Enter");
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 100, true),
           "...while the released Left is accepted again");

    /* Ten released taps 30 ms apart: ten accepts. */
    key_filter_init(&kf);
    int n = 0;
    for (uint32_t t = 0; t < 300; t += 30) {
        n += key_filter_accept(&kf, KF_KEY_ENTER, t, true);
        key_filter_release(&kf, KF_KEY_ENTER);
    }
    EXPECT(n == 10, "10 released taps 30 ms apart -> 10 accepts");

    /* Release, then a held key's repeats: the press acts once, the repeats
     * (no release between them) are dropped. */
    key_filter_init(&kf);
    key_filter_accept(&kf, KF_KEY_ESC, 0, true);
    key_filter_release(&kf, KF_KEY_ESC);
    EXPECT(hold_lvgl(&kf, KF_KEY_ESC, 100, 8, true) == 1,
           "hold right after a released tap fires once");

    /* Record-only (display locked) then release: the next press acts. */
    key_filter_init(&kf);
    key_filter_record(&kf, KF_KEY_ESC, 1000);
    EXPECT(!key_filter_accept(&kf, KF_KEY_ESC, 1100, true),
           "locked Esc, no release: its repeat is dropped");
    key_filter_release(&kf, KF_KEY_ESC);
    EXPECT(key_filter_accept(&kf, KF_KEY_ESC, 1150, true),
           "locked Esc released: the next press 50 ms later acts");

    /* A released key is a fresh key: the table slot is freed. */
    key_filter_init(&kf);
    key_filter_accept(&kf, KF_KEY_UP, 0, false);
    key_filter_release(&kf, KF_KEY_UP);
    int used = 0;
    for (int i = 0; i < KEY_FILTER_SLOTS; i++) used += kf.slots[i].used;
    EXPECT(used == 0, "release frees the key's slot");
}

/* ---------------------------------------------------------------------- */
/* Context changes, record-only path, independence                         */
/* ---------------------------------------------------------------------- */

static void test_cross_context_hold(void)
{
    /* Hold Enter on the switcher's Music tile: the press opens music, then
     * the context becomes music columns and the repeats must not play. */
    key_filter_t kf;
    key_filter_init(&kf);
    int n = key_filter_accept(&kf, KF_KEY_ENTER, 1000,
                              key_filter_is_one_shot(KF_KEY_ENTER, KEY_CTX_SWITCHER));
    EXPECT(n == 1, "Enter in switcher accepted");
    int later = 0;
    for (int i = 0; i < 10; i++)
        later += key_filter_accept(&kf, KF_KEY_ENTER, 1400 + 200u * (uint32_t)i,
                                   key_filter_is_one_shot(KF_KEY_ENTER,
                                                          KEY_CTX_MUSIC_BROWSE));
    EXPECT(later == 0, "held Enter repeats dropped after switch to music");

    /* A non-one-shot event (chat's ←) still records: a one-shot ← right
     * after it in music is dropped. */
    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 0,
                             key_filter_is_one_shot(KF_KEY_LEFT, KEY_CTX_CHAT)),
           "Left in chat accepted");
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 100,
                             key_filter_is_one_shot(KF_KEY_LEFT, KEY_CTX_CHAT)),
           "Left repeat in chat accepted (not one-shot)");
    EXPECT(!key_filter_accept(&kf, KF_KEY_LEFT, 300,
                              key_filter_is_one_shot(KF_KEY_LEFT,
                                                     KEY_CTX_MUSIC_BROWSE)),
           "one-shot Left within 450 ms of a recorded non-one-shot Left dropped");
}

static void test_record_only(void)
{
    /* Display locked: Esc held to wake is recorded, never acted on; after
     * the wake the repeats must still be dropped. */
    key_filter_t kf;
    key_filter_init(&kf);
    key_filter_record(&kf, KF_KEY_ESC, 2000);
    key_filter_record(&kf, KF_KEY_ESC, 2400);
    EXPECT(!key_filter_accept(&kf, KF_KEY_ESC, 2600, true),
           "one-shot Esc repeat after locked record dropped");
    EXPECT(key_filter_accept(&kf, KF_KEY_ESC, 3050, true),
           "Esc pressed again 450 ms later accepted");

    key_filter_init(&kf);
    key_filter_record(&kf, KF_KEY_ENTER, 0);
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, 450, true),
           "record-only then press at 450 accepted");
}

static void test_independent_keys(void)
{
    key_filter_t kf;
    key_filter_init(&kf);
    EXPECT(key_filter_accept(&kf, KF_KEY_LEFT, 0, true), "Left accepted");
    EXPECT(key_filter_accept(&kf, KF_KEY_RIGHT, 10, true), "Right right after Left accepted");
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, 20, true), "Enter right after accepted");
    EXPECT(!key_filter_accept(&kf, KF_KEY_LEFT, 100, true), "Left again still dropped");
    EXPECT(key_filter_accept(&kf, KF_KEY_ESC, 110, true), "Esc unaffected");
    /* Interleaved hold of two keys: each fires once. */
    key_filter_init(&kf);
    int l = 0, r = 0;
    for (uint32_t t = 0; t < 3000; t += 200) {
        l += key_filter_accept(&kf, KF_KEY_LEFT, t, true);
        r += key_filter_accept(&kf, KF_KEY_RIGHT, t + 5, true);
    }
    EXPECT(l == 1 && r == 1, "two interleaved held keys fire once each");
}

/* ---------------------------------------------------------------------- */
/* Tick wrap-around and table eviction                                     */
/* ---------------------------------------------------------------------- */

static void test_wraparound(void)
{
    key_filter_t kf;
    key_filter_init(&kf);
    uint32_t t0 = UINT32_MAX - 99;   /* 100 ms before the wrap */
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, t0, true), "press before wrap accepted");
    EXPECT(!key_filter_accept(&kf, KF_KEY_ENTER, t0 + 449u, true),
           "449 ms later across wrap dropped");
    key_filter_init(&kf);
    key_filter_accept(&kf, KF_KEY_ENTER, t0, true);
    EXPECT(key_filter_accept(&kf, KF_KEY_ENTER, t0 + 450u, true),
           "450 ms later across wrap accepted");
    key_filter_init(&kf);
    EXPECT(hold_lvgl(&kf, KF_KEY_SPACE, UINT32_MAX - 500, 20, true) == 1,
           "Pi hold spanning the wrap -> 1 accept");
}

static void test_table_full(void)
{
    key_filter_t kf;
    key_filter_init(&kf);

    /* Fill every slot with distinct keys at increasing times. */
    for (uint32_t i = 0; i < KEY_FILTER_SLOTS; i++)
        key_filter_accept(&kf, 1000 + i, 10 * i, false);
    uint32_t t = 10 * KEY_FILTER_SLOTS;

    /* A fresh key with a full table is still accepted. */
    EXPECT(key_filter_accept(&kf, KF_KEY_ESC, t, true), "fresh key accepted with full table");
    /* ...and it is now recorded, so its repeat is dropped. */
    EXPECT(!key_filter_accept(&kf, KF_KEY_ESC, t + 100, true), "fresh key's repeat dropped");
    /* The oldest entry (key 1000, t=0) was the one reused; the newest
     * filler key (recorded at t - 10) is still on record. */
    uint32_t newest = 1000 + KEY_FILTER_SLOTS - 1;
    EXPECT(!key_filter_accept(&kf, newest, t + 100, true),
           "newest filler key kept its record");
    EXPECT(key_filter_accept(&kf, 1000, t + 110, true),
           "evicted oldest key treated as fresh");

    /* Eviction picks the oldest across the wrap too. */
    key_filter_init(&kf);
    uint32_t base = UINT32_MAX - 200;
    for (uint32_t i = 0; i < KEY_FILTER_SLOTS; i++)
        key_filter_accept(&kf, 2000 + i, base + 10 * i, false); /* spans wrap */
    uint32_t now = base + 10 * KEY_FILTER_SLOTS;
    key_filter_accept(&kf, KF_KEY_ENTER, now, true);   /* evicts key 2000 */
    EXPECT(!key_filter_accept(&kf, 2000 + KEY_FILTER_SLOTS - 1, now + 1, true),
           "post-wrap newest entry kept across wrap");
    EXPECT(key_filter_accept(&kf, 2000, now + 2, true),
           "pre-wrap oldest entry was the one evicted");
}

int main(void)
{
    printf("=== key_filter unit tests ===\n");
    test_one_shot_table();
    test_boundary();
    test_hold_lvgl();
    test_hold_sdl();
    test_sliding();
    test_release_gap();
    test_release_resets();
    test_cross_context_hold();
    test_record_only();
    test_independent_keys();
    test_wraparound();
    test_table_full();

    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
