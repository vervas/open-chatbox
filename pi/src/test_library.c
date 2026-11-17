/**
 * test_library.c — Unit tests for the UI's library reader (library.{c,h},
 * docs/player-spec.md §2, §7.3–§7.7, §13 T6).
 *
 * Every DB is built by the real indexer scan (indexer_scan.c) with the stub
 * prober and thumbnailer from test_music_fixture.h, over a music tree made
 * at runtime under $TMPDIR.
 *
 * Run: ./test_library
 */

#include "indexer_scan.h"
#include "library.h"
#include "library_schema.h"
#include "test_music_fixture.h"
#include "text_nfc.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Fixture                                                                 */
/* ---------------------------------------------------------------------- */

static indexer_t *g_ix;

/* The lists are big; keep them out of the stack like the UI will. */
static library_artists_t       g_artists;
static library_albums_t        g_albums;
static library_all_albums_t    g_all;
static library_tracks_t        g_tracks;
static library_artist_tracks_t g_atracks;

static void ix_open(void)
{
    indexer_opts_t o = { .music_dir = g_music, .db_path = g_db, .art_dir = g_art,
                         .ops = &g_stub_ops };
    g_ix = indexer_open(&o);
    if (!g_ix) {
        fprintf(stderr, "indexer_open failed\n");
        exit(2);
    }
}

static void ix_close(void)
{
    indexer_close(g_ix);        /* NULL-safe */
    g_ix = NULL;
}

/* A fresh scratch tree with an (empty) indexed DB. */
static void setup(void)
{
    fixture_make_dirs("test_library");
    ix_open();
}

static void teardown(void)
{
    ix_close();
    fixture_remove();
}

static indexer_stats_t pass(void)
{
    indexer_stats_t st;
    memset(&st, 0, sizeof(st));
    if (indexer_pass(g_ix, &st) != 0)
        fprintf(stderr, "test: indexer pass failed\n");
    return st;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Names of a list joined with '|'. */
#define JOIN(list, field) join_((list).items[0].field, sizeof((list).items[0]), (list).count)
static const char *join_(const char *first, size_t stride, int count)
{
    static char buf[8192];
    size_t n = 0;
    buf[0] = '\0';
    for (int i = 0; i < count && n < sizeof(buf); i++)
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "|" : "",
                              first + (size_t)i * stride);
    return buf;
}

static int64_t artist_id(const char *name)
{
    for (int i = 0; i < g_artists.count; i++)
        if (strcmp(g_artists.items[i].name, name) == 0)
            return g_artists.items[i].id;
    return -1;
}

static int64_t album_id(const char *title)
{
    for (int i = 0; i < g_albums.count; i++)
        if (strcmp(g_albums.items[i].title, title) == 0)
            return g_albums.items[i].id;
    return -1;
}

static int64_t track_id(const char *title)
{
    for (int i = 0; i < g_tracks.count; i++)
        if (strcmp(g_tracks.items[i].title, title) == 0)
            return g_tracks.items[i].id;
    return -1;
}

/* A read-write side connection, as another process would have. */
static sqlite3 *rw_open(void)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(g_db, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        fprintf(stderr, "test: rw open failed\n");
        exit(2);
    }
    sqlite3_busy_timeout(db, 1000);
    return db;
}

/* Move everything into the main DB file and empty the WAL (keeping the
 * -wal/-shm files), so a test can corrupt the main file and be sure no
 * reader gets the pages from the WAL instead. No other connection open. */
static void checkpoint_truncate(void)
{
    sqlite3 *db = rw_open();
    int persist = 1;
    sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);
    if (sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL) != SQLITE_OK)
        fprintf(stderr, "test: checkpoint: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
}

/* The library used by most tests:
 *
 *   The Wiggly Bananas  (sort "wiggly bananas")
 *     Apple (2019), b side (2019), Fruit Salad Party (2021, cover.jpg),
 *     Zzz (no year)
 *   Zed Band / Double: two discs, two tracks without a number
 *   abba kids           (lower case: sorts first)
 *   Émile & Les Enfants (UTF-8; sorts after 'z', sort_name folds ASCII only)
 */
static void base_tree(void)
{
    put("The Wiggly Bananas/Fruit Salad Party/cover.jpg", "COVER-FSP");
    put("The Wiggly Bananas/Fruit Salad Party/01 Peel It Up.mp3",
        "title=Peel It Up\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\n"
        "track=1\nyear=2021\ndur=61000\n");
    put("The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3",
        "title=Monkey Business\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\n"
        "track=2\nyear=2021\ndur=72000\n");
    put("The Wiggly Bananas/b side/01 B1.mp3",
        "title=B1\nartist=The Wiggly Bananas\nalbum=b side\ntrack=1\nyear=2019\n");
    put("The Wiggly Bananas/Apple/01 A1.mp3",
        "title=A1\nartist=The Wiggly Bananas\nalbum=Apple\ntrack=1\nyear=2019\n");
    put("The Wiggly Bananas/Zzz/01 Z1.mp3",
        "title=Z1\nartist=The Wiggly Bananas\nalbum=Zzz\ntrack=1\n");

    put("Zed Band/Double/2-01 D2T1.mp3",
        "title=D2T1\nartist=Zed Band\nalbum=Double\ntrack=1\ndisc=2\n");
    put("Zed Band/Double/1-05 D1T5.mp3",
        "title=D1T5\nartist=Zed Band\nalbum=Double\ntrack=5\ndisc=1\n");
    put("Zed Band/Double/1-02 D1T2.mp3",
        "title=D1T2\nartist=Zed Band\nalbum=Double\ntrack=2\ndisc=1\n");
    put("Zed Band/Double/Bonus.mp3", "title=Bonus\nartist=Zed Band\nalbum=Double\n");
    put("Zed Band/Double/Another bonus.mp3",
        "title=Another bonus\nartist=Zed Band\nalbum=Double\n");

    put("abba kids/Songs/03 Three.ogg", "title=Three\nartist=abba kids\nalbum=Songs\n");

    put("Émile & Les Enfants/Chansons d'été/01 Ça va.mp3",
        "title=Ça va 🎵\nartist=Émile & Les Enfants\nalbum=Chansons d'été\n"
        "track=1\nyear=2020\nart=EMB-EMILE\n");
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                   */
/* ---------------------------------------------------------------------- */

static void test_utf8_copy(void)
{
    char buf[8];
    EXPECT(!library_utf8_copy(buf, sizeof(buf), "abc"), "short string not truncated");
    EXPECT_STR(buf, "abc", "short copy");
    EXPECT(!library_utf8_copy(buf, sizeof(buf), NULL), "NULL → not truncated");
    EXPECT_STR(buf, "", "NULL → empty");
    EXPECT(library_utf8_copy(buf, sizeof(buf), "abcdefghij"), "long ASCII truncated");
    EXPECT_STR(buf, "abcdefg", "ASCII cut at n-1");
    /* "abcdé" + "🎵": é = 2 bytes, 🎵 = 4 bytes. 7 bytes fit. */
    EXPECT(library_utf8_copy(buf, sizeof(buf), "abcd\xc3\xa9\xf0\x9f\x8e\xb5"), "cut");
    EXPECT_STR(buf, "abcd\xc3\xa9", "emoji dropped whole, é kept");
    EXPECT(library_utf8_copy(buf, 6, "abcd\xc3\xa9"), "é doesn't fit in 5");
    EXPECT_STR(buf, "abcd", "é dropped whole, never half");
    EXPECT(library_utf8_copy(buf, 5, "\xf0\x9f\x8e\xb5\xf0\x9f\x8e\xb5"), "two emoji in 4");
    EXPECT_STR(buf, "\xf0\x9f\x8e\xb5", "exactly one emoji");
    EXPECT(library_utf8_copy(buf, 4, "\xf0\x9f\x8e\xb5"), "emoji in 3");
    EXPECT_STR(buf, "", "nothing rather than a broken sequence");
}

/* Decomposed (NFD) text, as macOS writes names, composes for display. */
static void test_nfc(void)
{
    char s[64];
    snprintf(s, sizeof(s), "Re\xCC\x81" "flexion");                 /* e + U+0301 */
    text_utf8_nfc(s);
    EXPECT_STR(s, "R\xC3\xA9" "flexion", "nfc: e + acute -> \xC3\xA9");
    snprintf(s, sizeof(s), "C\xCC\xA7" "a");                     /* C + cedilla */
    text_utf8_nfc(s);
    EXPECT_STR(s, "\xC3\x87" "a", "nfc: C + cedilla");
    snprintf(s, sizeof(s), "\xCE\xB1\xCC\x81");                  /* alpha + acute */
    text_utf8_nfc(s);
    EXPECT_STR(s, "\xCE\xAC", "nfc: Greek alpha + acute -> alpha tonos");
    snprintf(s, sizeof(s), "\xCE\xB9\xCC\x88\xCC\x81");          /* iota + diaeresis + acute */
    text_utf8_nfc(s);
    EXPECT_STR(s, "\xCE\x90", "nfc: two marks compose in turn (U+0390)");
    snprintf(s, sizeof(s), "q\xCC\x81x");                       /* no precomposed q-acute */
    text_utf8_nfc(s);
    EXPECT_STR(s, "qx", "nfc: a mark with no composition is dropped");
    snprintf(s, sizeof(s), "\xC3\xA9t\xC3\xA9 \xE2\x80\x99 ok");    /* already NFC, U+2019 */
    text_utf8_nfc(s);
    EXPECT_STR(s, "\xC3\xA9t\xC3\xA9 \xE2\x80\x99 ok", "nfc: NFC text unchanged");

    /* The DB keeps names as on disk; the reader composes them. */
    setup();
    put("\xCE\xA4\xCE\xB1\xCC\x81\xCF\x83\xCE\xBF\xCF\x82/"
        "\xCE\xA7\xCE\xB1\xCF\x81\xCE\xB1\xCC\x81/01 \xCE\xA0\xCE\xBF\xCC\x81\xCE\xBC\xCE\xBF\xCE\xBB\xCE\xB1.mp3",
        "nothing=1\n");                                           /* Τάσος/Χαρά/01 Πόμολα, NFD */
    pass();
    sqlite3 *db = rw_open();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT name FROM artist", -1, &st, NULL);
    EXPECT(sqlite3_step(st) == SQLITE_ROW, "nfc: artist row");
    EXPECT_STR((const char *)sqlite3_column_text(st, 0),
               "\xCE\xA4\xCE\xB1\xCC\x81\xCF\x83\xCE\xBF\xCF\x82", "nfc: DB keeps the NFD name");
    sqlite3_finalize(st);
    sqlite3_close(db);

    library_t *lib = library_open(g_db, g_art);
    library_load_artists(lib, &g_artists);
    EXPECT_STR(JOIN(g_artists, name), "\xCE\xA4\xCE\xAC\xCF\x83\xCE\xBF\xCF\x82",
               "nfc: artist composed");
    library_load_albums(lib, g_artists.items[0].id, &g_albums);
    EXPECT_STR(JOIN(g_albums, title), "\xCE\xA7\xCE\xB1\xCF\x81\xCE\xAC", "nfc: album composed");
    library_load_tracks(lib, g_albums.items[0].id, &g_tracks);
    EXPECT_STR(JOIN(g_tracks, title), "\xCE\xA0\xCF\x8C\xCE\xBC\xCE\xBF\xCE\xBB\xCE\xB1",
               "nfc: track composed");
    library_track_info_t ti;
    library_track_get(lib, g_tracks.items[0].id, &ti);
    EXPECT_STR(ti.artist_name, "\xCE\xA4\xCE\xAC\xCF\x83\xCE\xBF\xCF\x82", "nfc: track_get artist");
    EXPECT(strstr(ti.path, "\xCE\xB1\xCC\x81") != NULL, "nfc: path stays raw (mpv opens it)");
    library_close(lib);
    teardown();
}

static void test_missing_then_appears(void)
{
    fixture_make_dirs("test_library");
    library_t *lib = library_open(g_db, g_art);
    EXPECT(lib != NULL, "open succeeds without a DB");
    EXPECT_INT(library_state(lib), LIBRARY_MISSING, "missing DB → MISSING");
    EXPECT_INT(library_generation(lib), -1, "no generation");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_ERR, "load without a DB → ERR");
    EXPECT_INT(g_artists.count, 0, "empty list");
    library_track_info_t ti;
    EXPECT_INT(library_track_get(lib, 1, &ti), LIBRARY_ERR, "track_get without a DB → ERR");
    EXPECT(!library_poll(lib), "poll: still missing → no change");
    EXPECT(!library_poll(lib), "poll again → no change");

    /* A zero-byte file (the indexer mid-create) is still "no library". */
    spit(g_db, "", 0);
    EXPECT(!library_poll(lib), "empty file → no change");
    EXPECT_INT(library_state(lib), LIBRARY_MISSING, "empty file → MISSING");
    unlink(g_db);

    /* The indexer creates the DB (schema, generation 0, no tracks). */
    ix_open();
    EXPECT(library_poll(lib), "DB appears → poll reports a change");
    EXPECT_INT(library_state(lib), LIBRARY_EMPTY, "no tracks → EMPTY");
    EXPECT_INT(library_generation(lib), 0, "generation 0");
    EXPECT(!library_poll(lib), "then no change");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "EMPTY loads fine");
    EXPECT_INT(g_artists.count, 0, "no artists");

    base_tree();
    indexer_stats_t st = pass();
    EXPECT(st.changed, "pass changed");
    EXPECT(library_poll(lib), "tracks arrive → change");
    EXPECT_INT(library_state(lib), LIBRARY_READY, "READY");
    EXPECT_INT(library_generation(lib), st.generation, "generation read");
    EXPECT(!library_poll(lib), "then no change");

    /* The DB deleted underneath the open reader → MISSING, reported once. */
    ix_close();
    char p[PATH_MAX];
    unlink(g_db);
    snprintf(p, sizeof(p), "%s-wal", g_db);
    unlink(p);
    snprintf(p, sizeof(p), "%s-shm", g_db);
    unlink(p);
    EXPECT(library_poll(lib), "DB deleted → change");
    EXPECT_INT(library_state(lib), LIBRARY_MISSING, "→ MISSING");
    EXPECT(!library_poll(lib), "reported once");

    /* Recreated (new inode, generation back to a low number) → reopened. */
    ix_open();
    pass();
    EXPECT(library_poll(lib), "recreated DB → change");
    EXPECT_INT(library_state(lib), LIBRARY_READY, "READY again");
    library_close(lib);
    teardown();
}

static void test_ordering(void)
{
    setup();
    base_tree();
    pass();
    library_t *lib = library_open(g_db, g_art);
    EXPECT_INT(library_state(lib), LIBRARY_READY, "ready");

    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "load artists");
    EXPECT_STR(JOIN(g_artists, name),
               "abba kids|The Wiggly Bananas|Zed Band|Émile & Les Enfants",
               "artists by sort_name ('The ' dropped, ASCII case-folded, UTF-8 last)");
    EXPECT_INT(g_artists.total, 4, "total");
    EXPECT_INT(g_artists.items[1].album_count, 4, "Wiggly: 4 albums");

    int64_t wb = artist_id("The Wiggly Bananas");
    EXPECT_INT(library_load_albums(lib, wb, &g_albums), LIBRARY_OK, "load albums");
    EXPECT_STR(JOIN(g_albums, title), "Apple|b side|Fruit Salad Party|Zzz",
               "albums by year, then title (case-insensitive), unknown year last");
    EXPECT_INT(g_albums.items[0].year, 2019, "year");
    EXPECT_INT(g_albums.items[3].year, 0, "unknown year → 0");
    EXPECT_INT(g_albums.items[2].track_count, 2, "track_count");
    EXPECT_INT(g_albums.items[2].artist_id, wb, "album's artist_id");

    EXPECT_INT(library_load_albums(lib, artist_id("Zed Band"), &g_albums), LIBRARY_OK, "zed");
    int64_t dbl = album_id("Double");
    EXPECT_INT(library_load_tracks(lib, dbl, &g_tracks), LIBRARY_OK, "load tracks");
    EXPECT_STR(JOIN(g_tracks, title), "D1T2|D1T5|Another bonus|Bonus|D2T1",
               "disc, then track_no with unknown last, then file name");
    EXPECT_INT(g_tracks.items[0].disc_no, 1, "disc 1");
    EXPECT_INT(g_tracks.items[4].disc_no, 2, "disc 2");
    EXPECT_INT(g_tracks.items[2].track_no, 0, "NULL track_no → 0");
    EXPECT_INT(g_tracks.items[1].track_no, 5, "track_no 5");
    EXPECT_INT(g_tracks.items[0].duration_ms, -1, "unknown duration → -1");
    EXPECT_INT(g_tracks.items[0].album_id, dbl, "track's album_id");

    /* Enter on an artist (§7.3): all albums in order, tracks in order. */
    EXPECT_INT(library_load_artist_tracks(lib, wb, &g_atracks), LIBRARY_OK, "artist tracks");
    EXPECT_STR(JOIN(g_atracks, title), "A1|B1|Peel It Up|Monkey Business|Z1",
               "artist tracks in album order, then track order");
    EXPECT_INT(g_atracks.items[2].duration_ms, 61000, "duration");

    /* Unknown parents are just empty. */
    EXPECT_INT(library_load_albums(lib, 99999, &g_albums), LIBRARY_OK, "unknown artist ok");
    EXPECT_INT(g_albums.count, 0, "unknown artist → no albums");
    EXPECT_INT(library_load_tracks(lib, 99999, &g_tracks), LIBRARY_OK, "unknown album ok");
    EXPECT_INT(g_tracks.count, 0, "unknown album → no tracks");

    /* UTF-8 round trip. */
    int64_t em = artist_id("Émile & Les Enfants");
    EXPECT(em > 0, "UTF-8 artist found");
    library_load_albums(lib, em, &g_albums);
    EXPECT_STR(g_albums.items[0].title, "Chansons d'été", "UTF-8 album title");
    library_load_tracks(lib, g_albums.items[0].id, &g_tracks);
    EXPECT_STR(g_tracks.items[0].title, "Ça va 🎵", "UTF-8 + emoji track title");

    library_close(lib);
    teardown();
}

/* The Albums column (§7.2): every album, by title ignoring case and a
 * leading "The ", then year (unknown last), then id. */
static void test_all_albums(void)
{
    setup();
    put("The Wiggly Bananas/Apple/01 a.mp3",
        "title=a\nartist=The Wiggly Bananas\nalbum=Apple\ntrack=1\nyear=2019\n");
    put("Zed Band/apple/01 a.mp3", "title=a\nartist=Zed Band\nalbum=apple\ntrack=1\nyear=2018\n");
    put("Zed Band/The Zoo/01 z.mp3", "title=z\nartist=Zed Band\nalbum=The Zoo\nyear=2020\n");
    put("Zed Band/zebra/01 z.mp3", "title=z\nartist=Zed Band\nalbum=zebra\nyear=2019\n");
    put("Zed Band/Theater/01 t.mp3", "title=t\nartist=Zed Band\nalbum=Theater\nyear=2019\n");
    put("Zed Band/the end/01 e.mp3", "title=e\nartist=Zed Band\nalbum=the end\nyear=2019\n");
    put("Band A/Greatest Hits/01 g.mp3", "title=g\nartist=Band A\nalbum=Greatest Hits\nyear=2015\n");
    put("Band A/Greatest Hits/02 g.mp3", "title=g2\nartist=Band A\nalbum=Greatest Hits\nyear=2015\n");
    put("Band B/Greatest Hits/01 g.mp3", "title=g\nartist=Band B\nalbum=Greatest Hits\n");
    put("Band C/Greatest Hits/01 g.mp3", "title=g\nartist=Band C\nalbum=Greatest Hits\nyear=2010\n");
    put("\xC3\x89mile/\xC3\x89t\xC3\xA9/01 e.mp3",
        "title=e\nartist=\xC3\x89mile\nalbum=\xC3\x89t\xC3\xA9\nyear=2001\n");
    put("Compilations/Bedtime/01 b.mp3", "title=b\nartist=Someone\nalbum=Bedtime\n");
    pass();
    library_t *lib = library_open(g_db, g_art);

    EXPECT_INT(library_load_all_albums(lib, &g_all), LIBRARY_OK, "load all albums");
    EXPECT_INT(g_all.total, 11, "every album of every artist");
    EXPECT_STR(JOIN(g_all, title),
               "apple|Apple|Bedtime|the end|Greatest Hits|Greatest Hits|Greatest Hits|"
               "Theater|zebra|The Zoo|\xC3\x89t\xC3\xA9",
               "by title (case-insensitive, leading 'The ' ignored, UTF-8 last)");
    EXPECT_INT(g_all.items[0].year, 2018, "same title, other case: older first");
    EXPECT_STR(g_all.items[0].artist, "Zed Band", "artist name filled");
    EXPECT_STR(g_all.items[1].artist, "The Wiggly Bananas", "...for each album");
    EXPECT_STR(g_all.items[2].artist, "Various Artists", "a compilation: Various Artists");
    EXPECT_INT(g_all.items[4].year, 2010, "same title: by year, 2010 first");
    EXPECT_STR(g_all.items[4].artist, "Band C", "...Band C");
    EXPECT_INT(g_all.items[5].year, 2015, "then 2015");
    EXPECT_INT(g_all.items[5].track_count, 2, "track_count");
    EXPECT_INT(g_all.items[6].year, 0, "unknown year last");
    EXPECT_STR(g_all.items[6].artist, "Band B", "...Band B");
    EXPECT(g_all.items[6].artist_id > 0, "artist_id filled");

    /* Ties (same title, both unknown year): by id. */
    put("Band D/Greatest Hits/01 g.mp3", "title=g\nartist=Band D\nalbum=Greatest Hits\n");
    pass();
    library_load_all_albums(lib, &g_all);
    EXPECT_STR(g_all.items[6].artist, "Band B", "unknown years tie: lower id first");
    EXPECT_STR(g_all.items[7].artist, "Band D", "...newer album after it");
    EXPECT(g_all.items[6].id < g_all.items[7].id, "ids ascending");

    /* find: by id, else the index clamped. */
    int64_t zoo = g_all.items[10].id;
    EXPECT_INT(library_all_albums_find(&g_all, zoo, 0), 10, "find by id");
    EXPECT_INT(library_all_albums_find(&g_all, 99999, 40), g_all.count - 1, "gone → clamped index");

    /* The per-artist list names the artist too. */
    library_load_artists(lib, &g_artists);
    library_load_albums(lib, artist_id("Zed Band"), &g_albums);
    EXPECT_STR(g_albums.items[0].artist, "Zed Band", "per-artist albums: artist name");
    library_close(lib);
    teardown();
}

static void test_art(void)
{
    setup();
    base_tree();
    put("Zed Band/Double/cover.png", "COVER-DBL");
    pass();
    library_t *lib = library_open(g_db, g_art);
    library_load_artists(lib, &g_artists);
    int64_t wb = artist_id("The Wiggly Bananas");
    library_load_albums(lib, wb, &g_albums);

    char p[PATH_MAX], want[PATH_MAX];
    const library_album_t *fsp = &g_albums.items[2];
    EXPECT_STR(fsp->title, "Fruit Salad Party", "fsp");
    EXPECT(fsp->art_id > 0, "cover → art_id");
    static const int sizes[] = LIBRARY_ART_SIZES;
    for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++) {
        EXPECT(library_art_path(lib, fsp->art_id, sizes[i], p, sizeof(p)), "art path");
        snprintf(want, sizeof(want), "%s/%lld_%d.png", g_art, (long long)fsp->art_id, sizes[i]);
        EXPECT_STR(p, want, "<art_dir>/<id>_<size>.png");
        struct stat st;
        EXPECT(stat(p, &st) == 0, "thumbnail exists");
        char *png = slurp(p, NULL);
        char tag[32];
        snprintf(tag, sizeof(tag), "PNG%d:", sizes[i]);
        EXPECT(png && strncmp(png, tag, strlen(tag)) == 0, "thumbnail of that size");
        free(png);
    }
    EXPECT(!library_art_path(lib, fsp->art_id, 100, p, sizeof(p)), "unsupported size → none");
    EXPECT_STR(p, "", "out cleared");
    EXPECT(!library_art_path(lib, fsp->art_id, 96, p, 8), "too small a buffer → none");

    const library_album_t *apple = &g_albums.items[0];
    EXPECT_INT(apple->art_id, 0, "no art → 0");
    EXPECT(!library_art_path(lib, apple->art_id, 96, p, sizeof(p)), "no art → none");

    /* Artist art = newest album with art (FSP 2021 > none). */
    EXPECT_INT(g_artists.items[1].art_id, fsp->art_id, "artist art from newest album with art");
    EXPECT_INT(g_artists.items[0].art_id, 0, "abba kids: no art");

    /* Embedded art (Émile) and the track's art via track_get. */
    int64_t em = artist_id("Émile & Les Enfants");
    library_load_albums(lib, em, &g_albums);
    EXPECT(g_albums.items[0].art_id > 0, "embedded art");
    library_load_tracks(lib, g_albums.items[0].id, &g_tracks);
    library_track_info_t ti;
    library_track_get(lib, g_tracks.items[0].id, &ti);
    EXPECT_INT(ti.art_id, g_albums.items[0].art_id, "track info carries the album art");

    /* No art_dir → never a path. */
    library_t *noart = library_open(g_db, NULL);
    EXPECT(!library_art_path(noart, fsp->art_id, 96, p, sizeof(p)), "no art_dir → none");
    library_close(noart);

    library_close(lib);
    teardown();
}

static void test_track_get(void)
{
    setup();
    base_tree();
    pass();
    library_t *lib = library_open(g_db, g_art);
    library_load_artists(lib, &g_artists);
    int64_t wb = artist_id("The Wiggly Bananas");
    library_load_albums(lib, wb, &g_albums);
    int64_t fsp = album_id("Fruit Salad Party");
    library_load_tracks(lib, fsp, &g_tracks);
    int64_t mb = track_id("Monkey Business");

    library_track_info_t ti;
    EXPECT_INT(library_track_get(lib, mb, &ti), LIBRARY_OK, "track_get");
    char want[PATH_MAX];
    abs_path(want, sizeof(want), "The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3");
    EXPECT_STR(ti.path, want, "absolute path");
    EXPECT_INT(ti.id, mb, "id");
    EXPECT_INT(ti.album_id, fsp, "album id");
    EXPECT_INT(ti.artist_id, wb, "artist id");
    EXPECT_INT(ti.duration_ms, 72000, "duration");
    EXPECT_INT(ti.track_no, 2, "track_no");
    EXPECT_INT(ti.disc_no, 1, "disc_no default 1");
    EXPECT_STR(ti.title, "Monkey Business", "title");
    EXPECT_STR(ti.album_title, "Fruit Salad Party", "album title");
    EXPECT_STR(ti.artist_name, "The Wiggly Bananas", "artist name");
    EXPECT(ti.artist_art_id > 0 && ti.artist_art_id == g_artists.items[1].art_id,
           "the artist's art id (for Now Playing's artist icon)");
    EXPECT_INT(library_track_get(lib, 99999, &ti), LIBRARY_NOT_FOUND, "unknown id");
    EXPECT_STR(ti.path, "", "cleared when not found");
    library_close(lib);
    teardown();
}

static void test_stable_ids_and_find(void)
{
    setup();
    base_tree();
    pass();
    library_t *lib = library_open(g_db, g_art);
    library_load_artists(lib, &g_artists);
    int64_t wb = artist_id("The Wiggly Bananas");
    library_load_albums(lib, wb, &g_albums);
    int64_t fsp = album_id("Fruit Salad Party");
    int64_t apple = album_id("Apple");
    library_load_tracks(lib, fsp, &g_tracks);
    int64_t peel = track_id("Peel It Up");
    int64_t mb = track_id("Monkey Business");

    /* Unchanged rescan: nothing moves, nothing reported. */
    indexer_stats_t st = pass();
    EXPECT(!st.changed, "unchanged rescan");
    EXPECT(!library_poll(lib), "unchanged rescan → no poll change");
    library_load_tracks(lib, fsp, &g_tracks);
    EXPECT_INT(track_id("Peel It Up"), peel, "track id kept");

    /* Retag a file (same path): track, album and artist ids survive. */
    put("The Wiggly Bananas/Fruit Salad Party/01 Peel It Up.mp3",
        "title=Peel It Up (2024 mix)\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\n"
        "track=1\nyear=2021\ndur=61000\n");
    pass();
    EXPECT(library_poll(lib), "retag → change");
    library_load_artists(lib, &g_artists);
    EXPECT_INT(artist_id("The Wiggly Bananas"), wb, "artist id kept across a retag");
    library_load_albums(lib, wb, &g_albums);
    EXPECT_INT(album_id("Fruit Salad Party"), fsp, "album id kept across a retag");
    library_load_tracks(lib, fsp, &g_tracks);
    EXPECT_INT(track_id("Peel It Up (2024 mix)"), peel, "track id kept across a retag");

    /* Rename a file: the track gets a new id, its album keeps its id. */
    mv("The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3",
       "The Wiggly Bananas/Fruit Salad Party/02 Monkey Business (renamed).mp3");
    pass();
    EXPECT(library_poll(lib), "rename → change");
    library_load_albums(lib, wb, &g_albums);
    EXPECT_INT(album_id("Fruit Salad Party"), fsp, "album id kept across a file rename");
    int sel = 1;    /* Monkey Business was selected at index 1 */
    library_load_tracks(lib, fsp, &g_tracks);
    int64_t mb2 = track_id("Monkey Business");
    EXPECT(mb2 > 0 && mb2 != mb, "renamed file → new track id");
    EXPECT_INT(library_tracks_find(&g_tracks, mb2, 0), 1, "find by id");
    EXPECT_INT(library_tracks_find(&g_tracks, mb, sel), sel,
               "old id gone → same index");
    EXPECT_INT(library_tracks_find(&g_tracks, mb, 7), 1, "fallback clamped to the list");
    EXPECT_INT(library_tracks_find(&g_tracks, mb, -3), 0, "negative fallback → 0");
    EXPECT_INT(library_tracks_find(&g_tracks, peel, 1), 0, "id beats the fallback");

    /* Rename the album directory: new album id; find falls back by index. */
    library_load_albums(lib, wb, &g_albums);
    int apple_idx = library_albums_find(&g_albums, apple, 0);
    EXPECT_INT(apple_idx, 0, "Apple at 0");
    mv("The Wiggly Bananas/Apple/01 A1.mp3", "The Wiggly Bananas/Apple Renamed/01 A1.mp3");
    char d[PATH_MAX];
    abs_path(d, sizeof(d), "The Wiggly Bananas/Apple");
    rmdir(d);
    pass();
    EXPECT(library_poll(lib), "dir rename → change");
    library_load_albums(lib, wb, &g_albums);
    EXPECT(album_id("Apple") != apple, "renamed dir → new album id");
    EXPECT_INT(library_albums_find(&g_albums, apple, apple_idx), apple_idx,
               "gone → same index");
    EXPECT_INT(library_albums_find(&g_albums, fsp, 0), 2, "kept album still found by id");

    /* Artists / artist-tracks find, and the empty list. */
    library_load_artists(lib, &g_artists);
    EXPECT_INT(library_artists_find(&g_artists, wb, 0), 1, "artist by id");
    library_load_artist_tracks(lib, wb, &g_atracks);
    EXPECT_INT(library_artist_tracks_find(&g_atracks, peel, 0), 2, "queue index by id");
    library_load_tracks(lib, 99999, &g_tracks);
    EXPECT_INT(library_tracks_find(&g_tracks, peel, 3), -1, "empty list → -1");

    /* Indexer restart: ids persist. */
    ix_close();
    ix_open();
    pass();
    EXPECT(!library_poll(lib), "indexer restart, no change");
    library_load_artists(lib, &g_artists);
    EXPECT_INT(artist_id("The Wiggly Bananas"), wb, "artist id survives an indexer restart");
    library_close(lib);
    teardown();
}

static void test_generation_once(void)
{
    setup();
    base_tree();
    pass();
    library_t *lib = library_open(g_db, g_art);
    int64_t g0 = library_generation(lib);
    EXPECT(!library_poll(lib), "fresh reader: nothing to report");

    put("New/N/01 n.mp3", "title=n1\n");
    put("New/N/02 n.mp3", "title=n2\n");
    rm("abba kids/Songs/03 Three.ogg");
    pass();
    EXPECT(library_poll(lib), "one pass, many changes → true");
    EXPECT_INT(library_generation(lib), g0 + 1, "generation +1");
    EXPECT(!library_poll(lib), "exactly once");
    pass();
    EXPECT(!library_poll(lib), "no-change pass → false");
    library_load_artists(lib, &g_artists);
    EXPECT_INT(artist_id("abba kids"), -1, "emptied artist gone after reload");
    EXPECT(artist_id("New") > 0, "new artist present after reload");
    library_close(lib);
    teardown();
}

static void test_readonly_and_wal(void)
{
    setup();
    base_tree();
    pass();
    library_t *lib = library_open(g_db, g_art);
    sqlite3 *rd = library_test_db(lib);
    EXPECT(rd != NULL, "reader open");
    EXPECT(sqlite3_exec(rd, "DELETE FROM track", NULL, NULL, NULL) != SQLITE_OK,
           "reader cannot write");
    EXPECT(sqlite3_db_readonly(rd, "main") == 1, "connection is read-only");
    library_load_artists(lib, &g_artists);
    EXPECT_INT(g_artists.count, 4, "nothing deleted");

    /* A writer in the middle of a transaction doesn't block the reader. */
    sqlite3 *w = rw_open();
    int64_t g0 = library_generation(lib);
    EXPECT(sqlite3_exec(w, "BEGIN IMMEDIATE; UPDATE meta SET value = CAST(value AS INTEGER) + 1"
                           " WHERE key = 'generation'; DELETE FROM track;",
                        NULL, NULL, NULL) == SQLITE_OK, "writer mid-transaction");
    double t0 = now_ms();
    EXPECT(!library_poll(lib), "uncommitted bump not visible");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "load during write");
    EXPECT_INT(g_artists.count, 4, "reader sees the committed snapshot");
    library_load_artists(lib, &g_artists);
    EXPECT(now_ms() - t0 < 40, "reading during a write never waits (WAL)");
    sqlite3_exec(w, "ROLLBACK", NULL, NULL, NULL);

    /* A real indexer pass committing while the reader is open. */
    put("More/M/01 m.mp3", "title=more\n");
    pass();
    EXPECT(library_poll(lib), "committed pass seen by the open reader");
    EXPECT_INT(library_generation(lib), g0 + 1, "exactly +1");
    sqlite3_close(w);

    /* The indexer closes while the reader is open: the reader keeps working. */
    ix_close();
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "reader after indexer close");
    EXPECT_INT(g_artists.count, 5, "5 artists");
    library_close(lib);

    /* No connection left at all (indexer stopped, UI restarted): a fresh
     * read-only reader still opens, because the indexer keeps -wal/-shm. */
    char wal[PATH_MAX], shm[PATH_MAX];
    snprintf(wal, sizeof(wal), "%s-wal", g_db);
    snprintf(shm, sizeof(shm), "%s-shm", g_db);
    struct stat sb;
    EXPECT(stat(wal, &sb) == 0 && stat(shm, &sb) == 0, "-wal/-shm persist after close");
    lib = library_open(g_db, g_art);
    EXPECT_INT(library_state(lib), LIBRARY_READY, "fresh reader without the indexer");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "fresh reader loads");
    EXPECT_INT(g_artists.count, 5, "5 artists");
    library_close(lib);

    /* Someone deleted -wal/-shm. Depending on the SQLite build a read-only
     * reader can't recreate them (Apple's: CANTOPEN) or can (upstream
     * unix VFS). Either way: no crash, and once the indexer runs the
     * library is READY. (Checkpoint first, so deleting the WAL loses
     * nothing.) */
    checkpoint_truncate();
    unlink(wal);
    unlink(shm);
    lib = library_open(g_db, g_art);
    if (library_state(lib) == LIBRARY_ERROR) {
        EXPECT(!library_poll(lib), "still no change");
        ix_open();
        EXPECT(library_poll(lib), "indexer starts → poll reports the library");
    } else {
        printf("  NOTE: this SQLite recreates -wal/-shm for a read-only reader\n");
        EXPECT_INT(library_state(lib), LIBRARY_READY, "no -wal/-shm → still READY");
        ix_open();
    }
    EXPECT_INT(library_state(lib), LIBRARY_READY, "READY");
    library_load_artists(lib, &g_artists);
    EXPECT_INT(g_artists.count, 5, "whole library back");
    library_close(lib);
    teardown();
}

static void test_busy_bounded(void)
{
    /* In WAL mode a writer never blocks the reader (test_readonly_and_wal).
     * To get a writer that really holds the DB, switch it to a rollback
     * journal and take an EXCLUSIVE lock: the worst case for busy_timeout. */
    setup();
    base_tree();
    pass();
    ix_close();
    sqlite3 *w = rw_open();
    sqlite3_busy_timeout(w, 0);
    EXPECT(sqlite3_exec(w, "PRAGMA journal_mode = DELETE", NULL, NULL, NULL) == SQLITE_OK,
           "rollback journal");
    library_t *lib = library_open(g_db, g_art);
    EXPECT_INT(library_state(lib), LIBRARY_READY, "ready");
    library_load_artists(lib, &g_artists);
    EXPECT_INT(g_artists.count, 4, "loaded");
    int64_t g0 = library_generation(lib);

    EXPECT(sqlite3_exec(w, "BEGIN EXCLUSIVE; UPDATE meta SET value = CAST(value AS INTEGER) + 1"
                           " WHERE key = 'generation';", NULL, NULL, NULL) == SQLITE_OK,
           "writer holds the DB exclusively");
    double t0 = now_ms();
    int rc = library_load_artists(lib, &g_artists);
    double dt = now_ms() - t0;
    EXPECT_INT(rc, LIBRARY_ERR, "load while held → ERR");
    EXPECT_INT(g_artists.count, 0, "list emptied on error");
    EXPECT(dt >= LIBRARY_BUSY_TIMEOUT_MS - 5, "it did wait for busy_timeout");
    EXPECT(dt < LIBRARY_BUSY_TIMEOUT_MS + 100, "load bounded by busy_timeout");
    EXPECT_INT(library_state(lib), LIBRARY_READY, "busy is not an error state");

    t0 = now_ms();
    EXPECT(!library_poll(lib), "poll while busy → false (reported later)");
    EXPECT(now_ms() - t0 < LIBRARY_BUSY_TIMEOUT_MS + 100, "poll bounded by busy_timeout");
    library_track_info_t ti;
    t0 = now_ms();
    EXPECT_INT(library_track_get(lib, 1, &ti), LIBRARY_ERR, "track_get busy → ERR");
    EXPECT(now_ms() - t0 < LIBRARY_BUSY_TIMEOUT_MS + 100, "track_get bounded");

    /* Released: the next poll reports the new generation, once. */
    sqlite3_exec(w, "COMMIT", NULL, NULL, NULL);
    EXPECT(library_poll(lib), "after release → reload");
    EXPECT_INT(library_generation(lib), g0 + 1, "new generation");
    EXPECT(!library_poll(lib), "reported once");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "load works again");
    EXPECT_INT(g_artists.count, 4, "all artists");

    /* A failed load alone (generation unchanged) still asks for a reload. */
    int64_t zed = artist_id("Zed Band");
    sqlite3_exec(w, "BEGIN EXCLUSIVE", NULL, NULL, NULL);
    EXPECT_INT(library_load_albums(lib, zed, &g_albums), LIBRARY_ERR, "busy load");
    sqlite3_exec(w, "COMMIT", NULL, NULL, NULL);
    EXPECT(library_poll(lib), "failed load → next poll asks for a reload");
    EXPECT_INT(library_generation(lib), g0 + 1, "generation unchanged");
    EXPECT(!library_poll(lib), "once");
    EXPECT_INT(library_load_albums(lib, zed, &g_albums), LIBRARY_OK, "retry works");
    EXPECT_INT(g_albums.count, 1, "Zed's album");

    sqlite3_close(w);
    library_close(lib);
    teardown();
}

static void test_corrupt(void)
{
    fixture_make_dirs("test_library");

    /* Garbage in place of the DB. */
    char junk[4096];
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = (char)('A' + i % 26);
    spit(g_db, junk, sizeof(junk));
    library_t *lib = library_open(g_db, g_art);
    EXPECT(lib != NULL, "open with a garbage file");
    EXPECT_INT(library_state(lib), LIBRARY_ERROR, "garbage → ERROR");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_ERR, "load → ERR");
    library_track_info_t ti;
    EXPECT_INT(library_track_get(lib, 1, &ti), LIBRARY_ERR, "track_get → ERR");
    EXPECT(!library_poll(lib), "still broken → no change");
    EXPECT(!library_poll(lib), "still broken → no change (no spam loop)");

    /* Wrong schema version. */
    unlink(g_db);
    ix_open();
    ix_close();
    sqlite3 *w = rw_open();
    sqlite3_exec(w, "PRAGMA user_version = 99", NULL, NULL, NULL);
    sqlite3_close(w);
    library_poll(lib);
    EXPECT_INT(library_state(lib), LIBRARY_ERROR, "schema version mismatch → ERROR");

    /* A real DB replacing it → READY. */
    char p[PATH_MAX];
    unlink(g_db);
    snprintf(p, sizeof(p), "%s-wal", g_db);
    unlink(p);
    snprintf(p, sizeof(p), "%s-shm", g_db);
    unlink(p);
    ix_open();
    base_tree();
    for (int i = 0; i < 40; i++) {     /* several pages of rows */
        char rel[64];
        snprintf(rel, sizeof(rel), "Pad/Album %02d/01 x.mp3", i);
        put(rel, "title=padding track with a reasonably long title to fill pages\n");
    }
    pass();
    EXPECT(library_poll(lib), "good DB → change");
    EXPECT_INT(library_state(lib), LIBRARY_READY, "READY");

    /* One corrupt page (the artist table's root) in an otherwise fine DB:
     * poll's status query still works, loading artists fails. The reader
     * goes to ERROR once and stays there (no READY/ERROR flapping and no
     * log line per poll) while the file and generation are unchanged. */
    sqlite3_stmt *q = NULL;
    sqlite3 *rd = library_test_db(lib);
    long long root = 0, page = 0;
    sqlite3_prepare_v2(rd, "SELECT rootpage FROM sqlite_master WHERE name = 'artist'",
                       -1, &q, NULL);
    if (sqlite3_step(q) == SQLITE_ROW)
        root = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    sqlite3_prepare_v2(rd, "PRAGMA page_size", -1, &q, NULL);
    if (sqlite3_step(q) == SQLITE_ROW)
        page = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    EXPECT(root > 1 && page >= 512, "artist root page found");
    library_close(lib);
    ix_close();
    checkpoint_truncate();
    FILE *f = fopen(g_db, "r+b");
    if (f) {
        fseek(f, (long)((root - 1) * page), SEEK_SET);
        for (long long i = 0; i < page; i++)
            fputc(0xA5, f);
        fclose(f);
    }
    lib = library_open(g_db, g_art);
    EXPECT_INT(library_state(lib), LIBRARY_READY, "status query doesn't touch that page");
    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_ERR, "corrupt page → ERR");
    EXPECT_INT(g_artists.count, 0, "nothing half-loaded");
    EXPECT_INT(library_state(lib), LIBRARY_ERROR, "→ ERROR");
    EXPECT(library_poll(lib), "the UI hears about it once");
    EXPECT(!library_poll(lib), "then nothing: no reopen-into-READY loop");
    EXPECT_INT(library_state(lib), LIBRARY_ERROR, "stays ERROR");
    EXPECT_INT(library_load_albums(lib, 1, &g_albums), LIBRARY_ERR, "loads keep failing cleanly");
    library_close(lib);

    /* Truncated to its first page: reads hit missing pages → ERROR. */
    if (truncate(g_db, 4096) != 0)
        perror("truncate");
    lib = library_open(g_db, g_art);
    EXPECT_INT(library_state(lib), LIBRARY_ERROR, "truncated DB → ERROR, not a crash");
    EXPECT_INT(library_load_artist_tracks(lib, 1, &g_atracks), LIBRARY_ERR, "load → ERR");
    EXPECT(!library_poll(lib), "no change while broken");

    /* Replaced by a good DB → READY. */
    char p2[PATH_MAX];
    unlink(g_db);
    snprintf(p2, sizeof(p2), "%s-wal", g_db);
    unlink(p2);
    snprintf(p2, sizeof(p2), "%s-shm", g_db);
    unlink(p2);
    ix_open();
    pass();
    EXPECT(library_poll(lib), "rebuilt DB → change");
    EXPECT_INT(library_state(lib), LIBRARY_READY, "READY again");
    library_close(lib);
    teardown();
}

static void test_caps(void)
{
    setup();
    /* One artist with LIBRARY_MAX_ALBUMS + 2 albums of one track, plus one
     * album with more than LIBRARY_MAX_TRACKS tracks, so the artist also
     * exceeds LIBRARY_MAX_ARTIST_TRACKS. */
    const int n_albums = LIBRARY_MAX_ALBUMS + 2;
    const int n_huge = LIBRARY_MAX_ARTIST_TRACKS - n_albums + 10;
    char rel[128], tags[128];
    for (int i = 0; i < n_albums; i++) {
        snprintf(rel, sizeof(rel), "Big/Album %03d/01 t.mp3", i);
        snprintf(tags, sizeof(tags), "artist=Big\nalbum=Album %03d\nyear=2000\n", i);
        put(rel, tags);
    }
    for (int i = 1; i <= n_huge; i++) {
        snprintf(rel, sizeof(rel), "Big/Zz Huge/%04d t.mp3", i);
        put(rel, "artist=Big\nalbum=Zz Huge\nyear=2001\n");
    }
    /* More artists than LIBRARY_MAX_ARTISTS. */
    const int n_artists = LIBRARY_MAX_ARTISTS + 3;
    for (int i = 0; i < n_artists - 1; i++) {
        snprintf(rel, sizeof(rel), "A%04d/Al/01 t.mp3", i);
        put(rel, "title=t\n");
    }
    pass();
    library_t *lib = library_open(g_db, g_art);

    EXPECT_INT(library_load_artists(lib, &g_artists), LIBRARY_OK, "capped artists ok");
    EXPECT_INT(g_artists.count, LIBRARY_MAX_ARTISTS, "artists capped");
    EXPECT_INT(g_artists.total, n_artists, "total says how many there are");
    EXPECT_STR(g_artists.items[0].name, "A0000", "first in order kept");
    EXPECT_STR(g_artists.items[LIBRARY_MAX_ARTISTS - 1].name, "A0511", "cut at the cap in order");

    /* "Big" sorts after "A…" and is past the cap: look it up by name. */
    int64_t big = -1;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(library_test_db(lib), "SELECT id FROM artist WHERE name = 'Big'",
                       -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        big = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    EXPECT(big > 0, "Big exists");

    EXPECT_INT(library_load_albums(lib, big, &g_albums), LIBRARY_OK, "capped albums ok");
    EXPECT_INT(g_albums.count, LIBRARY_MAX_ALBUMS, "albums capped");
    EXPECT_INT(g_albums.total, n_albums + 1, "album total");
    EXPECT_STR(g_albums.items[0].title, "Album 000", "first album kept");
    EXPECT_STR(g_albums.items[LIBRARY_MAX_ALBUMS - 1].title, "Album 127", "cut in order");

    /* Zz Huge (2001) sorts last, beyond the cap; find it via artist tracks. */
    EXPECT_INT(library_load_artist_tracks(lib, big, &g_atracks), LIBRARY_OK, "capped queue ok");
    EXPECT_INT(g_atracks.count, LIBRARY_MAX_ARTIST_TRACKS, "artist tracks capped");
    EXPECT_INT(g_atracks.total, n_albums + n_huge, "artist tracks total");
    int64_t huge = g_atracks.items[g_atracks.count - 1].album_id;
    EXPECT_INT(library_load_tracks(lib, huge, &g_tracks), LIBRARY_OK, "capped tracks ok");
    EXPECT_INT(g_tracks.count, LIBRARY_MAX_TRACKS, "tracks capped");
    EXPECT_INT(g_tracks.total, n_huge, "track total");
    EXPECT_INT(g_tracks.items[0].track_no, 1, "first track kept");
    EXPECT_INT(g_tracks.items[LIBRARY_MAX_TRACKS - 1].track_no, LIBRARY_MAX_TRACKS,
               "cut in order");
    /* A second load of the same capped list (held ↓) doesn't log again;
     * not asserted, visible in the output: one line per list. */
    library_load_tracks(lib, huge, &g_tracks);

    /* The Albums column: every album, capped at LIBRARY_MAX_ALL_ALBUMS. */
    EXPECT_INT(library_load_all_albums(lib, &g_all), LIBRARY_OK, "capped all albums ok");
    EXPECT_INT(g_all.count, LIBRARY_MAX_ALL_ALBUMS, "all albums capped");
    EXPECT_INT(g_all.total, n_albums + 1 + n_artists - 1, "all albums total");
    EXPECT_STR(g_all.items[0].title, "Al", "first in order kept ('Al' < 'Album 000')");
    library_close(lib);
    teardown();
}

int main(void)
{
    printf("=== library reader tests ===\n");
    printf("  sqlite %s\n", sqlite3_libversion());
    test_utf8_copy();
    test_nfc();
    test_missing_then_appears();
    test_ordering();
    test_all_albums();
    test_art();
    test_track_get();
    test_stable_ids_and_find();
    test_generation_once();
    test_readonly_and_wal();
    test_busy_bounded();
    test_corrupt();
    test_caps();

    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
