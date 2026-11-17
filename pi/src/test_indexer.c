/**
 * test_indexer.c — Unit tests for chatbox-indexer (docs/player-spec.md §2–§3).
 *
 * Every test builds its own music tree under $TMPDIR at runtime (no binary
 * fixtures). The prober and thumbnailer are stubs that read "media files"
 * made of key=value tag lines; see test_music_fixture.h (shared with
 * test_library).
 *
 * If real ffprobe/ffmpeg are on PATH, one extra integration test runs the
 * real implementation on a generated 1-second silent mp3 (skipped otherwise).
 *
 * Run: ./test_indexer
 */

#include "indexer_probe.h"
#include "indexer_scan.h"
#include "indexer_watch.h"
#include "library_schema.h"
#include "test_music_fixture.h"

#include <sqlite3.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Fixture (helpers and stubs: test_music_fixture.h)                      */
/* ---------------------------------------------------------------------- */

static indexer_t *g_ix;
static sqlite3 *g_rd;          /* independent reader connection */

static void setup(void)
{
    fixture_make_dirs("test_indexer");

    indexer_opts_t o = { .music_dir = g_music, .db_path = g_db, .art_dir = g_art,
                         .ops = &g_stub_ops };
    g_ix = indexer_open(&o);
    if (!g_ix) {
        fprintf(stderr, "indexer_open failed\n");
        exit(2);
    }
    if (sqlite3_open_v2(g_db, &g_rd, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "reader open failed\n");
        exit(2);
    }
    sqlite3_busy_timeout(g_rd, 50);
}

static void teardown(void)
{
    sqlite3_close(g_rd);
    g_rd = NULL;
    indexer_close(g_ix);
    g_ix = NULL;
    fixture_remove();
}

static int pass(indexer_stats_t *st)
{
    indexer_stats_t local;
    return indexer_pass(g_ix, st ? st : &local);
}

/* Integer result of a query (first column of the first row), -999 if none. */
static long long qi(const char *sql)
{
    sqlite3_stmt *s = NULL;
    long long v = -999;
    if (sqlite3_prepare_v2(g_rd, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "test sql error: %s: %s\n", sql, sqlite3_errmsg(g_rd));
        return -998;
    }
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(s, 0);
    else if (sqlite3_column_type(s, 0) == SQLITE_NULL)
        v = -1;     /* NULL */
    sqlite3_finalize(s);
    return v;
}

/* String result; rows joined with '|'. */
static const char *qs(const char *sql)
{
    static char buf[4096];
    buf[0] = '\0';
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(g_rd, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "test sql error: %s: %s\n", sql, sqlite3_errmsg(g_rd));
        return "SQLERR";
    }
    size_t n = 0;
    bool first = true;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(s, 0);
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s", first ? "" : "|",
                              t ? (const char *)t : "NULL");
        first = false;
        if (n >= sizeof(buf))
            break;
    }
    sqlite3_finalize(s);
    return buf;
}

static long long generation(void)
{
    return qi("SELECT CAST(value AS INTEGER) FROM meta WHERE key='generation'");
}

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool thumbs_present(long long art_id)
{
    static const int sizes[] = LIBRARY_ART_SIZES;
    for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%lld_%d.png", g_art, art_id, sizes[i]);
        if (!file_exists(p))
            return false;
    }
    return true;
}

static int count_art_files(void)
{
    int n = 0;
    for (long long id = 0; id < 64; id++) {
        static const int sizes[] = LIBRARY_ART_SIZES;
        for (int i = 0; i < LIBRARY_ART_SIZE_COUNT; i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%lld_%d.png", g_art, id, sizes[i]);
            if (file_exists(p))
                n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------- */
/* Pure helpers                                                            */
/* ---------------------------------------------------------------------- */

static void test_helpers(void)
{
    char out[64];
    indexer_sort_name("The Wiggly Bananas", out, sizeof(out));
    EXPECT_STR(out, "wiggly bananas", "sort_name drops 'The ' and folds case");
    indexer_sort_name("THE BAND", out, sizeof(out));
    EXPECT_STR(out, "band", "sort_name: 'THE ' any case");
    indexer_sort_name("Theatre Kids", out, sizeof(out));
    EXPECT_STR(out, "theatre kids", "sort_name keeps 'Theatre'");
    indexer_sort_name("The", out, sizeof(out));
    EXPECT_STR(out, "the", "sort_name: bare 'The' stays");

    EXPECT(indexer_is_media_name("01 a.mp3"), "mp3 is media");
    EXPECT(indexer_is_media_name("x.FLAC"), "FLAC (upper case) is media");
    EXPECT(indexer_is_media_name("x.m4a") && indexer_is_media_name("x.aac") &&
           indexer_is_media_name("x.ogg") && indexer_is_media_name("x.opus"),
           "m4a/aac/ogg/opus are media");
    EXPECT(!indexer_is_media_name(".hidden.mp3"), "hidden file ignored");
    EXPECT(!indexer_is_media_name("._x.mp3"), "AppleDouble ignored");
    EXPECT(!indexer_is_media_name("x.mp3.part"), ".part ignored");
    EXPECT(!indexer_is_media_name("x.mp3.download"), ".download ignored");
    EXPECT(!indexer_is_media_name("x.temp"), ".temp ignored");
    EXPECT(!indexer_is_media_name("x.temp.mp3"), "partial x.temp.mp3 ignored");
    EXPECT(!indexer_is_media_name("x.part.m4a"), "partial x.part.m4a ignored");
    EXPECT(!indexer_is_media_name("notes.txt"), "txt ignored");
    EXPECT(!indexer_is_media_name("x.wav"), "wav unsupported");
    EXPECT(!indexer_is_media_name("mp3"), "no extension ignored");

    EXPECT(indexer_cover_rank("cover.jpg") < indexer_cover_rank("folder.jpg"), "cover < folder");
    EXPECT(indexer_cover_rank("folder.png") < indexer_cover_rank("front.jpg"), "folder < front");
    EXPECT(indexer_cover_rank("cover.jpg") < indexer_cover_rank("cover.png"), "jpg < png");
    EXPECT(indexer_cover_rank("COVER.JPEG") >= 0, "case-insensitive");
    EXPECT(indexer_cover_rank("Front.Png") >= 0, "case-insensitive front");
    EXPECT_INT(indexer_cover_rank("cover.gif"), -1, "gif not a cover");
    EXPECT_INT(indexer_cover_rank("covers.jpg"), -1, "covers.jpg not a cover");
    EXPECT_INT(indexer_cover_rank("album.jpg"), -1, "album.jpg not a cover");

    /* SHA-1 known answer: "abc" */
    char p[PATH_MAX], sha[41];
    snprintf(p, sizeof(p), "%s/abc", g_root);
    spit(p, "abc", 3);
    EXPECT(indexer_sha1_file(p, sha) == 0, "sha1 of file");
    EXPECT_STR(sha, "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1('abc')");
    spit(p, "", 0);
    indexer_sha1_file(p, sha);
    EXPECT_STR(sha, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1('')");
}

static void test_parse_json(void)
{
    static const char *mp3 =
        "{\"streams\":[{\"codec_type\":\"audio\",\"duration\":\"61.5\"},"
        "{\"codec_type\":\"video\",\"disposition\":{\"attached_pic\":1}}],"
        "\"format\":{\"duration\":\"61.234\",\"tags\":{\"title\":\" Peel It Up \","
        "\"artist\":\"The Wiggly Bananas\",\"album_artist\":\"WB\",\"album\":\"Fruit\","
        "\"track\":\"3/12\",\"disc\":\"1/2\",\"date\":\"2019-05-01\"}}}";
    probe_tags_t t;
    EXPECT(indexer_probe_parse_json(mp3, &t) == 0, "parse mp3 json");
    EXPECT_STR(t.title, "Peel It Up", "title trimmed");
    EXPECT_STR(t.artist, "The Wiggly Bananas", "artist");
    EXPECT_STR(t.album_artist, "WB", "album_artist");
    EXPECT_STR(t.album, "Fruit", "album");
    EXPECT_INT(t.track_no, 3, "track 3/12");
    EXPECT_INT(t.disc_no, 1, "disc 1/2");
    EXPECT_INT(t.year, 2019, "year from date");
    EXPECT_INT(t.duration_ms, 61234, "format duration ms");
    EXPECT(t.has_art, "attached_pic → has_art");

    /* ogg/opus: tags on the audio stream, upper-case vorbis keys */
    static const char *ogg =
        "{\"streams\":[{\"codec_type\":\"audio\",\"duration\":\"2.5\",\"tags\":"
        "{\"TITLE\":\"Song\",\"ARTIST\":\"A\",\"ALBUMARTIST\":\"AA\",\"ALBUM\":\"Al\","
        "\"TRACKNUMBER\":\"7\",\"DISCNUMBER\":\"2\",\"DATE\":\"2001\"}},"
        "{\"codec_type\":\"video\",\"disposition\":{\"attached_pic\":0}}],"
        "\"format\":{}}";
    EXPECT(indexer_probe_parse_json(ogg, &t) == 0, "parse ogg json");
    EXPECT_STR(t.title, "Song", "stream TITLE");
    EXPECT_STR(t.album_artist, "AA", "ALBUMARTIST");
    EXPECT_INT(t.track_no, 7, "TRACKNUMBER");
    EXPECT_INT(t.disc_no, 2, "DISCNUMBER");
    EXPECT_INT(t.year, 2001, "DATE");
    EXPECT_INT(t.duration_ms, 2500, "stream duration fallback");
    EXPECT(!t.has_art, "video without attached_pic is not art");

    EXPECT(indexer_probe_parse_json("{\"streams\":[{\"codec_type\":\"video\"}],"
                                    "\"format\":{}}", &t) != 0, "no audio stream → fail");
    EXPECT(indexer_probe_parse_json("not json", &t) != 0, "garbage → fail");
}

/* ---------------------------------------------------------------------- */
/* Scheduler (fake clock)                                                  */
/* ---------------------------------------------------------------------- */

static void test_sched(void)
{
    indexer_sched_t s;
    const uint64_t R = 30u * 60u * 1000u;
    sched_init(&s, 0, INDEXER_DEBOUNCE_MS, R);
    EXPECT(sched_due(&s, 0), "startup pass due immediately");
    sched_pass_done(&s, 0);
    EXPECT(!sched_due(&s, 1), "nothing due after the startup pass");
    EXPECT_INT(sched_wait_ms(&s, 1000), R - 1000, "wait = until rescan");

    /* Events at 0..4 s → one pass at last event + 5 s. */
    for (uint64_t t = 1000; t <= 5000; t += 1000) {
        sched_event(&s, t);
        EXPECT(!sched_due(&s, t), "not due during the burst");
    }
    EXPECT_INT(sched_wait_ms(&s, 5000), 5000, "wait = debounce after last event");
    EXPECT(!sched_due(&s, 9999), "not due before last+5s");
    EXPECT(sched_due(&s, 10000), "due at last event + 5 s");
    sched_pass_done(&s, 10000);
    EXPECT(!sched_due(&s, 14999), "single pass for the burst");
    EXPECT(!sched_due(&s, 10000 + R - 1), "rescan not due early");
    EXPECT(sched_due(&s, 10000 + R), "full rescan every rescan_ms");
    sched_pass_done(&s, 10000 + R);

    /* Overflow → due right away. */
    sched_force(&s);
    EXPECT(sched_due(&s, 10000 + R + 1), "IN_Q_OVERFLOW forces a pass");
    EXPECT_INT(sched_wait_ms(&s, 10000 + R + 1), 0, "force → wait 0");
    sched_pass_done(&s, 10000 + R + 1);
    EXPECT(!sched_due(&s, 10000 + R + 2), "force cleared");

    /* Debounce never pushes past the periodic rescan. */
    sched_init(&s, 0, 5000, 10000);
    sched_pass_done(&s, 0);
    sched_event(&s, 8000);
    EXPECT_INT(sched_wait_ms(&s, 8000), 2000, "rescan earlier than debounce wins");
}

/* ---------------------------------------------------------------------- */
/* Scans                                                                   */
/* ---------------------------------------------------------------------- */

static void base_tree(void)
{
    put("The Wiggly Bananas/Fruit Salad Party/cover.jpg", "COVER-FSP");
    put("The Wiggly Bananas/Fruit Salad Party/01 Peel It Up.mp3",
        "title=Peel It Up\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\n"
        "track=1\nyear=2019\ndur=61000\n");
    put("The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3",
        "title=Monkey Business\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\n"
        "track=2\nyear=2019\ndur=72000\n");
    put("The Wiggly Bananas/Older Album/01 Old.mp3",
        "title=Old\nartist=The Wiggly Bananas\nalbum=Older Album\ntrack=1\nyear=2015\n"
        "art=EMB-OLD\n");
    put("Zed Band/Double/2-01 B.mp3",
        "title=B\nartist=Zed Band\nalbum=Double\ntrack=1\ndisc=2\n");
    put("Zed Band/Double/1-05 A.mp3",
        "title=A\nartist=Zed Band\nalbum=Double\ntrack=5\ndisc=1\n");
    put("Kid Band/Songs/03 Three.ogg", "title=Three\nartist=Kid Band\nalbum=Songs\ntrack=3\n");
}

static void test_first_scan(void)
{
    setup();
    base_tree();
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "first pass ok");
    EXPECT_INT(st.files, 6, "6 media files");
    EXPECT_INT(st.probed, 6, "all probed");
    EXPECT_INT(g_stub.probe_calls, 6, "stub probe calls");
    EXPECT_INT(st.added, 6, "6 added");
    EXPECT(st.changed, "first pass changed");
    EXPECT_INT(generation(), 1, "generation 1");
    EXPECT_INT(st.generation, 1, "stats.generation");

    EXPECT_STR(qs("SELECT name FROM artist ORDER BY sort_name"),
               "Kid Band|The Wiggly Bananas|Zed Band", "artists by sort_name");
    EXPECT_STR(qs("SELECT sort_name FROM artist WHERE name='The Wiggly Bananas'"),
               "wiggly bananas", "sort_name stored");
    EXPECT_STR(qs("SELECT al.title FROM album al JOIN artist ar ON ar.id=al.artist_id"
                  " WHERE ar.name='The Wiggly Bananas' ORDER BY al.year, al.title"),
               "Older Album|Fruit Salad Party", "albums by year then title");
    EXPECT_INT(qi("SELECT year FROM album WHERE title='Fruit Salad Party'"), 2019, "album year");
    EXPECT_STR(qs("SELECT dir FROM album WHERE title='Fruit Salad Party'"),
               "The Wiggly Bananas/Fruit Salad Party", "album.dir is the relative dir");
    EXPECT_STR(qs("SELECT t.title FROM track t JOIN album a ON a.id=t.album_id"
                  " WHERE a.title='Double' ORDER BY t.disc_no, t.track_no, t.path"),
               "A|B", "tracks by disc then track");
    EXPECT_INT(qi("SELECT duration_ms FROM track WHERE title='Peel It Up'"), 61000, "duration");
    EXPECT_INT(qi("SELECT track_no FROM track WHERE title='Monkey Business'"), 2, "track_no");
    EXPECT_INT(qi("SELECT disc_no FROM track WHERE title='Peel It Up'"), 1, "disc defaults to 1");
    EXPECT_INT(qi("SELECT duration_ms FROM track WHERE title='B'"), -1, "unknown duration NULL");

    char p[PATH_MAX], want[PATH_MAX + 64];
    abs_path(p, sizeof(p), "The Wiggly Bananas/Fruit Salad Party/01 Peel It Up.mp3");
    snprintf(want, sizeof(want), "SELECT count(*) FROM track WHERE path='%s'", p);
    EXPECT_INT(qi(want), 1, "track.path is absolute");
    struct stat sst;
    stat(p, &sst);
    EXPECT_INT(qi("SELECT size FROM track WHERE title='Peel It Up'"), sst.st_size, "size stored");

    /* WAL and schema version */
    EXPECT_STR(qs("PRAGMA journal_mode"), "wal", "WAL mode");
    EXPECT_INT(qi("PRAGMA user_version"), LIBRARY_SCHEMA_VERSION, "user_version");
    EXPECT_STR(qs("SELECT value FROM meta WHERE key='schema_version'"), "1", "meta.schema_version");
    EXPECT(qi("SELECT CAST(value AS INTEGER) FROM meta WHERE key='last_scan'") > 0, "last_scan set");
    teardown();
}

static void test_tags_beat_path(void)
{
    setup();
    put("Wrong Artist/Wrong Album/05 Wrong Title.mp3",
        "title=Right Title\nartist=Track Artist\nalbum_artist=Right Artist\nalbum=Right Album\ntrack=2\n");
    pass(NULL);
    EXPECT_STR(qs("SELECT name FROM artist"), "Right Artist", "album_artist beats artist and path");
    EXPECT_STR(qs("SELECT title FROM album"), "Right Album", "album tag beats dir");
    EXPECT_STR(qs("SELECT title FROM track"), "Right Title", "title tag beats file name");
    EXPECT_INT(qi("SELECT track_no FROM track"), 2, "track tag beats file number");
    teardown();

    setup();
    put("Wrong Artist/Wrong Album/05 x.mp3", "artist=Track Artist\n");
    pass(NULL);
    EXPECT_STR(qs("SELECT name FROM artist"), "Track Artist", "artist tag when no album_artist");
    EXPECT_STR(qs("SELECT title FROM album"), "Wrong Album", "missing album tag → dir");
    teardown();
}

static void test_path_fallback(void)
{
    setup();
    put("Zed Band/Tagless/03 Path Title.mp3", "nothing=1\n");
    put("Zed Band/Tagless/Plain Name.mp3", "nothing=1\n");
    put("Zed Band/Tagless/04 - Dashed.mp3", "nothing=1\n");
    put("Loose.mp3", "nothing=1\n");
    pass(NULL);
    EXPECT_STR(qs("SELECT name FROM artist ORDER BY sort_name"), "Unknown|Zed Band",
               "artist from path; root file → Unknown");
    EXPECT_STR(qs("SELECT title FROM album ORDER BY title"), "Tagless|Unknown", "album from dir");
    EXPECT_STR(qs("SELECT title FROM track WHERE path LIKE '%03 Path Title.mp3'"), "Path Title",
               "title from file name, number stripped");
    EXPECT_INT(qi("SELECT track_no FROM track WHERE path LIKE '%03 Path Title.mp3'"), 3,
               "track_no from file name");
    EXPECT_STR(qs("SELECT title FROM track WHERE path LIKE '%04 - Dashed.mp3'"), "Dashed",
               "'NN - Title'");
    EXPECT_STR(qs("SELECT title FROM track WHERE path LIKE '%Plain Name.mp3'"), "Plain Name",
               "no number: whole stem");
    EXPECT_INT(qi("SELECT track_no FROM track WHERE path LIKE '%Plain Name.mp3'"), -1,
               "no number: track_no NULL");
    EXPECT_INT(qi("SELECT disc_no FROM track WHERE path LIKE '%03 Path Title.mp3'"), 1, "disc 1");
    teardown();
}

static void test_inbox(void)
{
    setup();
    put("_inbox/random video.mp3", "nothing=1\n");
    put("_inbox/Tagged One.mp3", "title=Tagged\nartist=Kid Band\n");
    put("_inbox/Some Channel/clip.mp3", "nothing=1\n");
    pass(NULL);
    EXPECT_STR(qs("SELECT ar.name FROM track t JOIN album al ON al.id=t.album_id"
                  " JOIN artist ar ON ar.id=al.artist_id WHERE t.title='random video'"),
               "Unknown", "_inbox untagged → Unknown");
    EXPECT_STR(qs("SELECT al.title FROM track t JOIN album al ON al.id=t.album_id"
                  " WHERE t.title='random video'"), "Inbox", "_inbox album 'Inbox'");
    EXPECT_STR(qs("SELECT ar.name FROM track t JOIN album al ON al.id=t.album_id"
                  " JOIN artist ar ON ar.id=al.artist_id WHERE t.title='Tagged'"),
               "Kid Band", "_inbox tagged keeps its artist");
    EXPECT_STR(qs("SELECT ar.name FROM track t JOIN album al ON al.id=t.album_id"
                  " JOIN artist ar ON ar.id=al.artist_id WHERE t.title='clip'"),
               "Unknown", "_inbox subdir untagged → Unknown (not the dir name)");
    EXPECT_INT(qi("SELECT count(*) FROM album"), 3, "inbox groups by tags: separate albums");
    teardown();
}

static void test_compilations(void)
{
    setup();
    put("Compilations/Bedtime Songs/01 Lullaby.mp3",
        "title=Lullaby\nartist=Someone\nalbum_artist=Someone\nalbum=Bedtime Songs\n");
    put("Compilations/Bedtime Songs/02 Sleep.mp3", "title=Sleep\nartist=Other\n");
    pass(NULL);
    EXPECT_STR(qs("SELECT name FROM artist"), "Various Artists", "Compilations → Various Artists");
    EXPECT_INT(qi("SELECT count(*) FROM album"), 1, "one album");
    EXPECT_STR(qs("SELECT title FROM album"), "Bedtime Songs", "album title");
    teardown();
}

static long long art_of_album(const char *title)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT art_id FROM album WHERE title='%s'", title);
    return qi(sql);
}

static const char *src_of_album(const char *title)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT art.src FROM album JOIN art ON art.id=album.art_id"
                               " WHERE album.title='%s'", title);
    return qs(sql);
}

static void test_art_order(void)
{
    setup();
    put("X/A/cover.jpg", "IMG-A-COVER");
    put("X/A/folder.jpg", "IMG-A-FOLDER");
    put("X/A/front.png", "IMG-A-FRONT");
    put("X/A/01 a.mp3", "album=A\nart=EMB-A\n");
    put("X/B/FOLDER.JPG", "IMG-B-FOLDER");
    put("X/B/Front.png", "IMG-B-FRONT");
    put("X/B/01 b.mp3", "album=B\n");
    put("X/C/Front.PNG", "IMG-C-FRONT");
    put("X/C/01 c.mp3", "album=C\n");
    put("X/D/cover.png", "IMG-D-PNG");
    put("X/D/Cover.Jpeg", "IMG-D-JPEG");
    put("X/D/01 d.mp3", "album=D\n");
    pass(NULL);

    EXPECT_STR(src_of_album("A"), "X/A/cover.jpg", "cover beats folder, front and embedded");
    EXPECT_STR(src_of_album("B"), "X/B/FOLDER.JPG", "FOLDER.JPG beats Front.png");
    EXPECT_STR(src_of_album("C"), "X/C/Front.PNG", "Front.PNG found case-insensitively");
    EXPECT_STR(src_of_album("D"), "X/D/Cover.Jpeg", "jpeg beats png");
    EXPECT_INT(g_stub.extract_calls, 0, "no embedded extraction when a cover exists");

    long long a = art_of_album("A");
    EXPECT(a > 0 && thumbs_present(a), "three thumbnails for art A");
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%lld_256.png", g_art, a);
    char *t = slurp(p, NULL);
    EXPECT_STR(t, "PNG256:IMG-A-COVER", "thumbnail made from the cover at size 256");
    free(t);
    EXPECT_INT(g_stub.thumb_calls, 4 * LIBRARY_ART_SIZE_COUNT, "3 thumbnails per art");
    EXPECT_INT(count_art_files(), 4 * LIBRARY_ART_SIZE_COUNT, "12 files in art dir");

    char sha[41];
    abs_path(p, sizeof(p), "X/A/cover.jpg");
    indexer_sha1_file(p, sha);
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT sha1 FROM art WHERE id=%lld", a);
    EXPECT_STR(qs(sql), sha, "art.sha1 = sha1 of source bytes");
    teardown();

    /* Identical covers are stored once. */
    setup();
    put("Y/One/cover.jpg", "SAME");
    put("Y/One/01.mp3", "album=One\n");
    put("Y/Two/cover.jpg", "SAME");
    put("Y/Two/01.mp3", "album=Two\n");
    pass(NULL);
    EXPECT_INT(qi("SELECT count(*) FROM art"), 1, "dedup by sha1");
    EXPECT(art_of_album("One") == art_of_album("Two"), "both albums share the art row");
    teardown();
}

static void test_embedded_and_none(void)
{
    setup();
    put("E/Emb/01 first.mp3", "album=Emb\ntrack=1\n");
    put("E/Emb/02 second.mp3", "album=Emb\ntrack=2\nart=EMB-2\n");
    put("E/Emb/03 third.mp3", "album=Emb\ntrack=3\nart=EMB-3\n");
    put("E/None/01 x.mp3", "album=None\n");
    pass(NULL);
    EXPECT_STR(src_of_album("Emb"), "embedded:E/Emb/02 second.mp3",
               "embedded art from the first track that has it");
    EXPECT_INT(g_stub.extract_calls, 1, "tracks known to have no art aren't extracted");
    long long a = art_of_album("Emb");
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%lld_96.png", g_art, a);
    char *t = slurp(p, NULL);
    EXPECT_STR(t, "PNG96:EMB-2", "thumbnail of the embedded picture");
    free(t);
    EXPECT_INT(art_of_album("None"), -1, "no art → art_id NULL");
    EXPECT_INT(qi("SELECT count(*) FROM art"), 1, "one art row");

    /* Adding a cover later: cover wins, the embedded art row goes away. */
    int thumbs = g_stub.thumb_calls;
    put("E/Emb/cover.jpg", "LATE-COVER");
    indexer_stats_t st;
    pass(&st);
    EXPECT(st.changed, "new cover changes the library");
    EXPECT_STR(src_of_album("Emb"), "E/Emb/cover.jpg", "cover added later beats embedded");
    EXPECT_INT(g_stub.thumb_calls - thumbs, LIBRARY_ART_SIZE_COUNT, "thumbnails for new cover");
    EXPECT_INT(qi("SELECT count(*) FROM art"), 1, "old embedded art row removed");
    EXPECT(!thumbs_present(a) || art_of_album("Emb") == a, "old thumbnails removed");

    /* Removing the cover: back to embedded art. */
    rm("E/Emb/cover.jpg");
    pass(&st);
    EXPECT_STR(src_of_album("Emb"), "embedded:E/Emb/02 second.mp3", "cover gone → embedded again");
    teardown();

    /* A cover that can't be thumbnailed falls through to embedded art. */
    setup();
    put("F/Bad/cover.jpg", "BADIMG");
    put("F/Bad/01.mp3", "album=Bad\nart=EMB-OK\n");
    pass(NULL);
    EXPECT_STR(src_of_album("Bad"), "embedded:F/Bad/01.mp3", "bad cover → embedded");
    int thumbs2 = g_stub.thumb_calls;
    indexer_stats_t st2;
    pass(&st2);
    EXPECT(!st2.changed, "bad cover: stable on rescan");
    EXPECT_INT(g_stub.thumb_calls, thumbs2, "bad cover not retried until it changes");
    teardown();
}

static void test_artist_art_newest(void)
{
    setup();
    put("Art/Old/cover.jpg", "OLD");
    put("Art/Old/01.mp3", "artist=Art\nalbum=Old\nyear=2010\n");
    put("Art/New/cover.jpg", "NEW");
    put("Art/New/01.mp3", "artist=Art\nalbum=New\nyear=2020\n");
    put("Art/Newest No Art/01.mp3", "artist=Art\nalbum=Newest No Art\nyear=2022\n");
    put("Plain/P/01.mp3", "artist=Plain\nalbum=P\n");
    pass(NULL);
    EXPECT_INT(qi("SELECT art_id FROM artist WHERE name='Art'"), art_of_album("New"),
               "artist art = newest album with art");
    EXPECT_INT(qi("SELECT art_id FROM artist WHERE name='Plain'"), -1, "no album art → NULL");

    /* The newest album with art goes → the next one. */
    rm_tree(g_music);
    mkdir(g_music, 0755);
    put("Art/Old/cover.jpg", "OLD");
    put("Art/Old/01.mp3", "artist=Art\nalbum=Old\nyear=2010\n");
    pass(NULL);
    EXPECT_INT(qi("SELECT art_id FROM artist WHERE name='Art'"), art_of_album("Old"),
               "artist art follows when the newest goes");
    teardown();
}

static void test_unchanged_rescan(void)
{
    setup();
    base_tree();
    pass(NULL);
    long long gen = generation();
    stub_t before = g_stub;
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "second pass ok");
    EXPECT(!st.changed, "unchanged pass: no change");
    EXPECT_INT(st.probed, 0, "unchanged pass probes nothing");
    EXPECT_INT(g_stub.probe_calls, before.probe_calls, "no stub probe calls");
    EXPECT_INT(g_stub.extract_calls, before.extract_calls, "no extract calls");
    EXPECT_INT(g_stub.thumb_calls, before.thumb_calls, "no thumbnail calls");
    EXPECT_INT(generation(), gen, "generation not bumped");
    teardown();
}

static void test_modified_file(void)
{
    setup();
    base_tree();
    pass(NULL);
    long long gen = generation();
    int probes = g_stub.probe_calls;
    put("The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3",
        "title=Monkey Business (Remix)\nartist=The Wiggly Bananas\nalbum=Fruit Salad Party\n"
        "track=2\nyear=2019\ndur=80000\n");
    indexer_stats_t st;
    pass(&st);
    EXPECT_INT(g_stub.probe_calls - probes, 1, "only the changed file probed");
    EXPECT(strstr(g_stub.last_probed, "02 Monkey Business.mp3") != NULL, "it's the changed file");
    EXPECT_INT(st.updated, 1, "1 updated");
    EXPECT_INT(generation(), gen + 1, "generation +1");
    EXPECT_INT(qi("SELECT count(*) FROM track WHERE title='Monkey Business (Remix)'"), 1,
               "new title stored");
    EXPECT_INT(qi("SELECT count(*) FROM track"), 6, "no duplicate row");

    /* Same size, only mtime changed → still re-probed. */
    probes = g_stub.probe_calls;
    char p[PATH_MAX];
    abs_path(p, sizeof(p), "Kid Band/Songs/03 Three.ogg");
    struct timeval tv[2] = { { 1800000000, 0 }, { 1800000000, 0 } };
    utimes(p, tv);
    pass(&st);
    EXPECT_INT(g_stub.probe_calls - probes, 1, "mtime change → re-probed");
    teardown();
}

static void test_deletes(void)
{
    setup();
    base_tree();
    pass(NULL);
    long long gen = generation();
    long long old_art = art_of_album("Older Album");
    EXPECT(old_art > 0 && thumbs_present(old_art), "Older Album has embedded art");

    rm("The Wiggly Bananas/Fruit Salad Party/02 Monkey Business.mp3");
    indexer_stats_t st;
    pass(&st);
    EXPECT_INT(st.removed, 1, "one removed");
    EXPECT_INT(qi("SELECT count(*) FROM track WHERE title='Monkey Business'"), 0, "row gone");
    EXPECT_INT(qi("SELECT count(*) FROM album WHERE title='Fruit Salad Party'"), 1, "album stays");
    EXPECT_INT(generation(), gen + 1, "generation +1");

    char d[PATH_MAX];
    abs_path(d, sizeof(d), "The Wiggly Bananas/Older Album");
    rm_tree(d);
    abs_path(d, sizeof(d), "Kid Band");
    rm_tree(d);
    pass(&st);
    EXPECT_INT(qi("SELECT count(*) FROM album WHERE title='Older Album'"), 0, "empty album gone");
    EXPECT_INT(qi("SELECT count(*) FROM artist WHERE name='Kid Band'"), 0, "empty artist gone");
    EXPECT_INT(qi("SELECT count(*) FROM album WHERE title='Songs'"), 0, "its album gone");
    EXPECT_INT(qi("SELECT count(*) FROM artist WHERE name='The Wiggly Bananas'"), 1,
               "artist with albums left stays");
    EXPECT_INT(qi("SELECT art_id FROM artist WHERE name='The Wiggly Bananas'"),
               art_of_album("Fruit Salad Party"), "artist art moves to the remaining album");
    EXPECT(!thumbs_present(old_art) || old_art == art_of_album("Fruit Salad Party"),
           "orphan thumbnails deleted");
    EXPECT_INT(qi("SELECT count(*) FROM art"), 1, "orphan art row deleted");
    EXPECT_INT(count_art_files(), LIBRARY_ART_SIZE_COUNT, "only live thumbnails remain");
    teardown();
}

static void test_rename_move(void)
{
    setup();
    base_tree();
    pass(NULL);
    long long gen = generation();
    mv("Kid Band/Songs/03 Three.ogg", "Kid Band/Songs/03 Three Renamed.ogg");
    indexer_stats_t st;
    pass(&st);
    EXPECT_INT(st.added, 1, "rename: one added");
    EXPECT_INT(st.removed, 1, "rename: one removed");
    EXPECT_INT(qi("SELECT count(*) FROM track WHERE path LIKE '%03 Three.ogg'"), 0, "old path gone");
    EXPECT_INT(qi("SELECT count(*) FROM track WHERE path LIKE '%03 Three Renamed.ogg'"), 1,
               "new path present");
    EXPECT_INT(qi("SELECT count(*) FROM track"), 6, "count unchanged");
    EXPECT_INT(generation(), gen + 1, "generation +1");

    /* Move a whole untagged album to another artist folder. */
    put("Moved/Alb/01 m.mp3", "nothing=1\n");
    pass(NULL);
    EXPECT_INT(qi("SELECT count(*) FROM artist WHERE name='Moved'"), 1, "path artist 'Moved'");
    char a[PATH_MAX], b[PATH_MAX];
    abs_path(a, sizeof(a), "Moved");
    abs_path(b, sizeof(b), "Relocated");
    rename(a, b);
    pass(NULL);
    EXPECT_INT(qi("SELECT count(*) FROM artist WHERE name='Moved'"), 0, "old artist gone");
    EXPECT_INT(qi("SELECT count(*) FROM artist WHERE name='Relocated'"), 1, "new artist");
    EXPECT_STR(qs("SELECT dir FROM album WHERE title='Alb'"), "Relocated/Alb", "album dir updated");
    teardown();
}

static void test_ignored(void)
{
    setup();
    put("A/B/.hidden.mp3", "title=x\n");
    put("A/B/._x.mp3", "title=x\n");
    put("A/B/x.mp3.part", "title=x\n");
    put("A/B/x.temp.mp3", "title=x\n");
    put("A/B/x.mp3.download", "title=x\n");
    put("A/B/x.temp", "title=x\n");
    put("A/B/notes.txt", "title=x\n");
    put("A/B/x.wav", "title=x\n");
    put("A/.hidden dir/y.mp3", "title=x\n");
    put("A/B/empty.mp3", "");
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "pass ok");
    EXPECT_INT(qi("SELECT count(*) FROM track"), 0, "nothing indexed");
    EXPECT_INT(st.files, 1, "only empty.mp3 counted as media");
    EXPECT_INT(g_stub.probe_calls, 0, "ignored files (and an empty file) never probed");
    EXPECT_INT(generation(), 0, "no change, generation stays 0");
    teardown();
}

static void test_probe_failure(void)
{
    setup();
    put("P/Q/01 good.mp3", "title=Good\n");
    put("P/Q/02 bad.mp3", "fail=1\n");
    put("P/Q/03 fine.mp3", "title=Fine\n");
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "pass with a bad file still commits");
    EXPECT_INT(st.probe_failed, 1, "one probe failure");
    EXPECT_INT(qi("SELECT count(*) FROM track"), 2, "the other files are indexed");
    EXPECT_INT(generation(), 1, "generation 1");

    int probes = g_stub.probe_calls;
    pass(&st);
    EXPECT_INT(g_stub.probe_calls - probes, 1, "failed file retried next pass");
    EXPECT(!st.changed, "still failing: no change");
    EXPECT_INT(generation(), 1, "generation unchanged");

    /* A changed file whose probe fails keeps its old row. */
    put("P/Q/01 good.mp3", "title=Broken now\nfail=1\n");
    pass(&st);
    EXPECT_STR(qs("SELECT title FROM track WHERE path LIKE '%01 good.mp3'"), "Good",
               "changed + failing file keeps old row");
    EXPECT(!st.changed, "no change recorded");
    put("P/Q/01 good.mp3", "title=Good again\n");
    pass(&st);
    EXPECT_STR(qs("SELECT title FROM track WHERE path LIKE '%01 good.mp3'"), "Good again",
               "fixed file picked up");
    EXPECT_INT(generation(), 2, "generation 2");
    teardown();
}

static void test_generation_once_per_pass(void)
{
    setup();
    base_tree();
    pass(NULL);
    long long gen = generation();
    put("New/N/01.mp3", "title=n1\n");
    put("New/N/02.mp3", "title=n2\n");
    put("New/N/cover.jpg", "NCOVER");
    rm("Zed Band/Double/2-01 B.mp3");
    put("Kid Band/Songs/03 Three.ogg", "title=Three v2\nartist=Kid Band\nalbum=Songs\n");
    indexer_stats_t st;
    pass(&st);
    EXPECT_INT(generation(), gen + 1, "many changes, one pass → +1");
    pass(&st);
    EXPECT_INT(generation(), gen + 1, "then stable");
    teardown();
}

static void test_concurrent_reader(void)
{
    setup();
    base_tree();
    pass(NULL);
    /* A second connection holds a read transaction across the pass. */
    sqlite3 *r = NULL;
    sqlite3_open_v2(g_db, &r, SQLITE_OPEN_READONLY, NULL);
    sqlite3_busy_timeout(r, 50);
    sqlite3_exec(r, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(r, "SELECT count(*) FROM track", -1, &s, NULL);
    sqlite3_step(s);
    int before = sqlite3_column_int(s, 0);
    sqlite3_reset(s);

    put("More/M/01.mp3", "title=more\n");
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "pass commits while a reader holds a transaction");
    EXPECT(st.changed, "changed");

    sqlite3_step(s);
    EXPECT_INT(sqlite3_column_int(s, 0), before, "reader's snapshot unchanged inside its txn");
    sqlite3_reset(s);
    sqlite3_exec(r, "COMMIT", NULL, NULL, NULL);
    sqlite3_step(s);
    EXPECT_INT(sqlite3_column_int(s, 0), before + 1, "reader sees the new track afterwards");
    sqlite3_finalize(s);

    /* The reader cannot write. */
    EXPECT(sqlite3_exec(r, "DELETE FROM track", NULL, NULL, NULL) != SQLITE_OK,
           "read-only connection can't write");
    sqlite3_close(r);
    teardown();
}

static void test_art_orphans(void)
{
    setup();
    put("O/Al/cover.jpg", "V1");
    put("O/Al/01.mp3", "album=Al\n");
    pass(NULL);
    long long a1 = art_of_album("Al");
    EXPECT(thumbs_present(a1), "v1 thumbnails");

    /* Junk in the art dir: stale temp and an orphan id. Other files stay. */
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/.tmp-junk", g_art);
    spit(p, "x", 1);
    snprintf(p, sizeof(p), "%s/999_96.png", g_art);
    spit(p, "x", 1);
    char keep[PATH_MAX];
    snprintf(keep, sizeof(keep), "%s/README", g_art);
    spit(keep, "x", 1);

    put("O/Al/cover.jpg", "V2-different");
    indexer_stats_t st;
    pass(&st);
    long long a2 = art_of_album("Al");
    EXPECT(a2 > 0 && thumbs_present(a2), "v2 thumbnails");
    EXPECT(st.changed, "new cover bytes → change");
    EXPECT_INT(qi("SELECT count(*) FROM art"), 1, "old art row gone");
    EXPECT_INT(count_art_files(), LIBRARY_ART_SIZE_COUNT, "only v2 thumbnails remain");
    EXPECT(!file_exists(p), "orphan 999_96.png deleted");
    snprintf(p, sizeof(p), "%s/.tmp-junk", g_art);
    EXPECT(!file_exists(p), "stale temp deleted");
    EXPECT(file_exists(keep), "unrelated file kept");

    /* Thumbnails wiped from disk → regenerated. */
    static const int sizes[] = LIBRARY_ART_SIZES;
    snprintf(p, sizeof(p), "%s/%lld_%d.png", g_art, a2, sizes[1]);
    unlink(p);
    pass(&st);
    EXPECT(thumbs_present(art_of_album("Al")), "missing thumbnail regenerated");
    teardown();
}

static void test_symlink_loop(void)
{
    setup();
    put("L/Alb/01.mp3", "title=one\n");
    char link[PATH_MAX], target[PATH_MAX];
    abs_path(link, sizeof(link), "L/Alb/loop");
    snprintf(target, sizeof(target), "..");
    EXPECT(symlink(target, link) == 0, "make loop symlink");
    abs_path(link, sizeof(link), "L/self");
    EXPECT(symlink(".", link) == 0, "make self symlink");
    abs_path(link, sizeof(link), "L/Alb/root");
    EXPECT(symlink(g_music, link) == 0, "make root symlink");
    abs_path(link, sizeof(link), "L/dangling");
    symlink("/nonexistent/nowhere", link);
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "pass completes despite symlink loops");
    EXPECT_INT(qi("SELECT count(*) FROM track"), 1, "track indexed once");
    teardown();
}

static void test_missing_music_dir(void)
{
    setup();
    base_tree();
    pass(NULL);
    long long gen = generation();
    char moved[PATH_MAX];
    snprintf(moved, sizeof(moved), "%s/elsewhere", g_root);
    rename(g_music, moved);
    indexer_stats_t st;
    EXPECT_INT(pass(&st), -1, "missing music dir → pass aborted");
    EXPECT_INT(qi("SELECT count(*) FROM track"), 6, "library not wiped");
    EXPECT_INT(generation(), gen, "generation unchanged");
    rename(moved, g_music);
    teardown();
}

static void test_reopen(void)
{
    /* Reopening an existing DB keeps it; a rescan after restart probes nothing. */
    setup();
    base_tree();
    pass(NULL);
    indexer_close(g_ix);
    indexer_opts_t o = { .music_dir = g_music, .db_path = g_db, .art_dir = g_art,
                         .ops = &g_stub_ops };
    g_ix = indexer_open(&o);
    EXPECT(g_ix != NULL, "reopen");
    int probes = g_stub.probe_calls;
    int thumbs = g_stub.thumb_calls;
    indexer_stats_t st;
    pass(&st);
    EXPECT_INT(g_stub.probe_calls, probes, "restart: no re-probe");
    EXPECT_INT(g_stub.thumb_calls, thumbs, "restart: no re-thumbnail");
    EXPECT(!st.changed, "restart: no change");
    teardown();
}

/* ---------------------------------------------------------------------- */
/* Optional: real ffprobe/ffmpeg                                           */
/* ---------------------------------------------------------------------- */

static bool have_tool(const char *name)
{
    char *argv[] = { (char *)name, "-version", NULL };
    return indexer_run(argv, NULL, 10000) == 0;
}

static void test_real_ffmpeg(void)
{
    if (!have_tool("ffprobe") || !have_tool("ffmpeg")) {
        printf("  SKIP real ffprobe/ffmpeg test (not on PATH)\n");
        return;
    }
    setup();
    char dir[PATH_MAX], out[PATH_MAX], urlout[PATH_MAX + 8];
    abs_path(dir, sizeof(dir), "Real \"Artist\"/It's Album ü");
    abs_path(out, sizeof(out), "Real \"Artist\"/It's Album ü/01 Silent Song.mp3");
    mkdirs_for(out);
    snprintf(urlout, sizeof(urlout), "file:%s", out);
    char *argv[] = { "ffmpeg", "-nostdin", "-v", "error", "-y",
                     "-f", "lavfi", "-t", "1", "-i", "anullsrc=r=44100:cl=mono",
                     "-f", "lavfi", "-t", "1", "-i", "color=c=red:s=64x48:r=1",
                     "-map", "0:a", "-map", "1:v",
                     "-c:a", "libmp3lame", "-c:v", "mjpeg", "-disposition:v", "attached_pic",
                     "-metadata", "title=Silent Song", "-metadata", "artist=Real Artist",
                     "-metadata", "album=Quiet", "-metadata", "track=1/1",
                     "-metadata", "date=2024", "-id3v2_version", "3", urlout, NULL };
    if (indexer_run(argv, NULL, 30000) != 0) {
        printf("  SKIP real ffmpeg test (could not generate an mp3: no libmp3lame?)\n");
        teardown();
        return;
    }
    const indexer_probe_ops_t *ff = indexer_probe_ffmpeg();
    probe_tags_t t;
    EXPECT_INT(ff->probe(ff->ctx, out, &t), 0, "real ffprobe");
    EXPECT_STR(t.title, "Silent Song", "real title");
    EXPECT_STR(t.artist, "Real Artist", "real artist");
    EXPECT_INT(t.track_no, 1, "real track");
    EXPECT_INT(t.year, 2024, "real year");
    EXPECT(t.duration_ms > 900 && t.duration_ms < 1200, "real duration ≈ 1 s");
    EXPECT(t.has_art, "real attached picture detected");

    /* Full pass with the real implementation. */
    indexer_close(g_ix);
    indexer_opts_t o = { .music_dir = g_music, .db_path = g_db, .art_dir = g_art, .ops = ff };
    g_ix = indexer_open(&o);
    indexer_stats_t st;
    EXPECT_INT(pass(&st), 0, "real pass");
    EXPECT_STR(qs("SELECT title FROM track"), "Silent Song", "real track row");
    long long a = art_of_album("Quiet");
    EXPECT(a > 0 && thumbs_present(a), "real thumbnails from embedded art");
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%lld_512.png", g_art, a);
    size_t n = 0;
    char *png = slurp(p, &n);
    EXPECT(png && n > 8 && memcmp(png, "\x89PNG", 4) == 0, "real thumbnail is a PNG");
    free(png);
    teardown();
}

int main(void)
{
    printf("=== chatbox-indexer tests ===\n");
    setup();                /* for test_helpers' scratch files */
    test_helpers();
    teardown();
    test_parse_json();
    test_sched();
    test_first_scan();
    test_tags_beat_path();
    test_path_fallback();
    test_inbox();
    test_compilations();
    test_art_order();
    test_embedded_and_none();
    test_artist_art_newest();
    test_unchanged_rescan();
    test_modified_file();
    test_deletes();
    test_rename_move();
    test_ignored();
    test_probe_failure();
    test_generation_once_per_pass();
    test_concurrent_reader();
    test_art_orphans();
    test_symlink_loop();
    test_missing_music_dir();
    test_reopen();
    test_real_ffmpeg();

    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
