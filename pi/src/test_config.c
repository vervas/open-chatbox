/**
 * test_config.c — config parser tests
 *
 * Usage:
 *   test_config                 run the assertion-based self-tests (exit 1 on failure)
 *   test_config <config-file>   parse the file and print every value (CLI mode,
 *                               used by docker-compose's `test` service)
 *
 * The self-tests write small config files to $TMPDIR (or /tmp) and check
 * what config_parse makes of them, including the D9 music/shell keys and
 * the documented parser quirks (docs/player-spec.md §10). Warnings are
 * checked by capturing stderr to a temp file around each parse.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static int g_tests = 0;
static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_INT(got, want, msg) do { \
    int _g = (got), _w = (want); \
    g_tests++; \
    if (_g != _w) { \
        fprintf(stderr, "FAIL [%s:%d] %s: got %d, want %d\n", \
                __FILE__, __LINE__, msg, _g, _w); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_STR(got, want, msg) do { \
    const char *_g = (got), *_w = (want); \
    g_tests++; \
    if (strcmp(_g, _w) != 0) { \
        fprintf(stderr, "FAIL [%s:%d] %s: got '%s', want '%s'\n", \
                __FILE__, __LINE__, msg, _g, _w); \
        g_failures++; \
    } \
} while (0)

/* Minimal valid header: config_parse requires these two keys */
#define HDR "homeserver_url = http://hs:8008\naccess_token = tok\n"

static char g_tmpdir[512];
static char g_stderr_buf[4096];   /* stderr captured during the last parse */

/* Write `body` to a fresh temp file, parse it with stderr captured into
 * g_stderr_buf, delete the file. Returns config_parse's result. */
static int parse_str(const char *body, config_t *cfg)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/test_config_XXXXXX", g_tmpdir);
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    FILE *f = fdopen(fd, "w");
    fputs(body, f);
    fclose(f);

    char errpath[600];
    snprintf(errpath, sizeof(errpath), "%s/test_config_err_XXXXXX", g_tmpdir);
    int efd = mkstemp(errpath);
    if (efd < 0) { perror("mkstemp"); exit(2); }

    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(efd, STDERR_FILENO);
    int rc = config_parse(path, cfg);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    lseek(efd, 0, SEEK_SET);
    ssize_t n = read(efd, g_stderr_buf, sizeof(g_stderr_buf) - 1);
    g_stderr_buf[n > 0 ? n : 0] = '\0';
    close(efd);
    unlink(errpath);
    unlink(path);
    return rc;
}

static int warned(const char *needle)
{
    return strstr(g_stderr_buf, needle) != NULL;
}

/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    config_t cfg;
    EXPECT_INT(parse_str(HDR, &cfg), 0, "defaults: parse ok");
    EXPECT_STR(cfg.music_dir,     "/srv/music",                  "default music_dir");
    EXPECT_STR(cfg.music_db,      "/var/lib/chatbox/library.db", "default music_db");
    EXPECT_STR(cfg.music_art_dir, "/var/lib/chatbox/art",        "default music_art_dir");
    EXPECT_INT(cfg.music_volume_max,     70, "default music_volume_max");
    EXPECT_INT(cfg.music_volume_default, 40, "default music_volume_default");
    EXPECT_INT(cfg.default_app, CONFIG_APP_CHAT, "default default_app");
    EXPECT(g_stderr_buf[0] == '\0', "defaults: no warnings");
}

static void test_every_key(void)
{
    config_t cfg;
    EXPECT_INT(parse_str(HDR
        "music_dir = /data/tunes\n"
        "music_db = /tmp/lib.db\n"
        "music_art_dir = /tmp/art dir\n"
        "music_volume_max = 90\n"
        "music_volume_default = 55\n"
        "default_app = music\n", &cfg), 0, "every key: parse ok");
    EXPECT_STR(cfg.music_dir,     "/data/tunes", "music_dir parsed");
    EXPECT_STR(cfg.music_db,      "/tmp/lib.db", "music_db parsed");
    EXPECT_STR(cfg.music_art_dir, "/tmp/art dir", "music_art_dir parsed (inner space kept)");
    EXPECT_INT(cfg.music_volume_max,     90, "music_volume_max parsed");
    EXPECT_INT(cfg.music_volume_default, 55, "music_volume_default parsed");
    EXPECT_INT(cfg.default_app, CONFIG_APP_MUSIC, "default_app = music");
    EXPECT(g_stderr_buf[0] == '\0', "every key: no warnings");

    EXPECT_INT(parse_str(HDR "default_app = chat\n", &cfg), 0, "default_app chat: ok");
    EXPECT_INT(cfg.default_app, CONFIG_APP_CHAT, "default_app = chat");

    EXPECT_INT(parse_str(HDR "default_app = home\n", &cfg), 0, "default_app home: ok");
    EXPECT_INT(cfg.default_app, CONFIG_APP_HOME, "default_app = home");
    EXPECT(g_stderr_buf[0] == '\0', "default_app home: no warning");

    /* Edge values are in range */
    parse_str(HDR "music_volume_max = 100\nmusic_volume_default = 0\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     100, "max = 100 kept");
    EXPECT_INT(cfg.music_volume_default, 0,   "default = 0 kept");
    parse_str(HDR "music_volume_max = +0\nmusic_volume_default = 0\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     0, "max = +0 parsed");
    EXPECT(g_stderr_buf[0] == '\0', "edge values: no warnings");
}

static void test_clamping(void)
{
    config_t cfg;

    parse_str(HDR "music_volume_max = 150\nmusic_volume_default = 120\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     100, "max 150 -> 100");
    EXPECT_INT(cfg.music_volume_default, 100, "default 120 -> 100");
    EXPECT(warned("out of range"), "clamp high warns");

    parse_str(HDR "music_volume_max = -5\nmusic_volume_default = -1\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     0, "max -5 -> 0");
    EXPECT_INT(cfg.music_volume_default, 0, "default -1 -> 0");

    /* Overflowing numbers saturate, then clamp */
    parse_str(HDR "music_volume_max = 99999999999999999999\n"
                  "music_volume_default = -99999999999999999999\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     100, "huge max -> 100");
    EXPECT_INT(cfg.music_volume_default, 0,   "huge negative default -> 0");

    /* default > max -> default = max, in either key order */
    parse_str(HDR "music_volume_max = 30\nmusic_volume_default = 50\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     30, "max 30 kept");
    EXPECT_INT(cfg.music_volume_default, 30, "default 50 > max 30 -> 30");
    EXPECT(warned("music_volume_default 50 > music_volume_max 30"), "default > max warns");

    parse_str(HDR "music_volume_default = 50\nmusic_volume_max = 30\n", &cfg);
    EXPECT_INT(cfg.music_volume_default, 30, "default > max, reversed key order -> 30");

    /* Only max set, below the built-in default of 40 */
    parse_str(HDR "music_volume_max = 20\n", &cfg);
    EXPECT_INT(cfg.music_volume_default, 20, "built-in default 40 > max 20 -> 20");

    /* default == max is fine */
    parse_str(HDR "music_volume_max = 60\nmusic_volume_default = 60\n", &cfg);
    EXPECT_INT(cfg.music_volume_default, 60, "default == max kept");
    EXPECT(g_stderr_buf[0] == '\0', "default == max: no warnings");
}

static void test_garbage_ints(void)
{
    static const char *bad[] = { "loud", "70%", "7 0", "0x40", "", "70.5", "- 5" };
    config_t cfg;
    char body[256], msg[128];

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(body, sizeof(body), HDR "music_volume_max = %s\nmusic_volume_default = %s\n",
                 bad[i], bad[i]);
        int rc = parse_str(body, &cfg);
        snprintf(msg, sizeof(msg), "garbage '%s': parse still ok", bad[i]);
        EXPECT_INT(rc, 0, msg);
        snprintf(msg, sizeof(msg), "garbage '%s': max keeps default 70", bad[i]);
        EXPECT_INT(cfg.music_volume_max, 70, msg);
        snprintf(msg, sizeof(msg), "garbage '%s': default keeps default 40", bad[i]);
        EXPECT_INT(cfg.music_volume_default, 40, msg);
        snprintf(msg, sizeof(msg), "garbage '%s': warns", bad[i]);
        EXPECT(warned("is not a number"), msg);
    }

    /* Garbage in one key doesn't disturb a valid other key */
    parse_str(HDR "music_volume_max = loud\nmusic_volume_default = 25\n", &cfg);
    EXPECT_INT(cfg.music_volume_max,     70, "garbage max, valid default: max 70");
    EXPECT_INT(cfg.music_volume_default, 25, "garbage max, valid default: default 25");

    /* Empty path keys keep the default */
    parse_str(HDR "music_dir =\n", &cfg);
    EXPECT_STR(cfg.music_dir, "/srv/music", "empty music_dir keeps default");
    EXPECT(warned("music_dir is empty"), "empty music_dir warns");
}

static void test_default_app_unknown(void)
{
    static const char *bad[] = { "games", "Music", "", "music chat" };
    config_t cfg;
    char body[256], msg[128];

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(body, sizeof(body), HDR "default_app = %s\n", bad[i]);
        int rc = parse_str(body, &cfg);
        snprintf(msg, sizeof(msg), "default_app '%s': parse ok", bad[i]);
        EXPECT_INT(rc, 0, msg);
        snprintf(msg, sizeof(msg), "default_app '%s' -> chat", bad[i]);
        EXPECT_INT(cfg.default_app, CONFIG_APP_CHAT, msg);
        snprintf(msg, sizeof(msg), "default_app '%s' warns", bad[i]);
        EXPECT(warned("unknown default_app"), msg);
    }

    /* An unknown value after a valid one still falls back to chat */
    parse_str(HDR "default_app = music\ndefault_app = games\n", &cfg);
    EXPECT_INT(cfg.default_app, CONFIG_APP_CHAT, "music then unknown -> chat");
}

/* Documented parser quirks (spec §10): these assert current behaviour. */
static void test_quirks(void)
{
    config_t cfg;

    /* Keys under a [music] / [shell] header are plain global keys */
    EXPECT_INT(parse_str(HDR
        "[music]\n"
        "music_volume_max = 80\n"
        "music_dir = /m\n"
        "[shell]\n"
        "default_app = music\n", &cfg), 0, "[music] header: parse ok");
    EXPECT_INT(cfg.music_volume_max, 80, "[music] header: key still read");
    EXPECT_STR(cfg.music_dir, "/m",      "[music] header: path still read");
    EXPECT_INT(cfg.default_app, CONFIG_APP_MUSIC, "[shell] header: key still read");

    /* Keys after [contacts] become (legacy) contacts, not music keys */
    EXPECT_INT(parse_str(HDR
        "[contacts]\n"
        "Mom = !mom:localhost\n"
        "music_volume_max = 10\n"
        "default_app = music\n", &cfg), 0, "after [contacts]: parse ok");
    EXPECT_INT(cfg.music_volume_max, 70, "after [contacts]: max stays default");
    EXPECT_INT(cfg.default_app, CONFIG_APP_CHAT, "after [contacts]: default_app stays chat");
    EXPECT_INT(cfg.contact_count, 3, "after [contacts]: music keys counted as contacts");
    EXPECT_STR(cfg.contacts[1].name,    "music_volume_max", "after [contacts]: contact name = key");
    EXPECT_STR(cfg.contacts[1].room_id, "10",               "after [contacts]: room_id = value");
    EXPECT_STR(cfg.contacts[2].name,    "default_app",      "after [contacts]: 2nd stray key");

    /* No inline comments: a ';' or '#' mid-line stays in the value */
    parse_str(HDR "music_dir = /srv/music ; where songs live\n"
                  "music_db = /x.db # db\n", &cfg);
    EXPECT_STR(cfg.music_dir, "/srv/music ; where songs live", "inline ; comment stays in value");
    EXPECT_STR(cfg.music_db,  "/x.db # db",                    "inline # comment stays in value");

    /* ...which makes an inline comment on an int key garbage -> default */
    parse_str(HDR "music_volume_max = 50 ; percent\n", &cfg);
    EXPECT_INT(cfg.music_volume_max, 70, "int with inline comment -> default");
    EXPECT(warned("is not a number"), "int with inline comment warns");

    /* Full-line comments, including indented ones, are ignored */
    parse_str(HDR "# music_volume_max = 10\n   # default_app = music\n", &cfg);
    EXPECT_INT(cfg.music_volume_max, 70, "commented-out key ignored");
    EXPECT_INT(cfg.default_app, CONFIG_APP_CHAT, "indented comment ignored");
}

static void test_existing_keys(void)
{
    config_t cfg;

    EXPECT_INT(parse_str(
        "# comment\n"
        "homeserver_url = http://mock:8008\n"
        "access_token = test_token_123\n"
        "sleep_lock_secs = 90\n"
        "sleep_off_secs = 600\n"
        "music_volume_max = 80\n"
        "\n"
        "[contacts]\n"
        "contact.1.name = Mom\n"
        "contact.1.room_id = !testroom:localhost\n"
        "contact.1.avatar = mom\n"
        "contact.2.name = Dad\n"
        "contact.2.room_id = !room2:localhost\n", &cfg), 0, "existing: parse ok");
    EXPECT_STR(cfg.homeserver_url, "http://mock:8008", "homeserver_url");
    EXPECT_STR(cfg.access_token,   "test_token_123",   "access_token");
    EXPECT_INT(cfg.sleep_lock_secs, 90,  "sleep_lock_secs");
    EXPECT_INT(cfg.sleep_off_secs,  600, "sleep_off_secs");
    EXPECT_INT(cfg.music_volume_max, 80, "music key alongside existing keys");
    EXPECT_INT(cfg.contact_count, 2, "contact_count (new format)");
    EXPECT_STR(cfg.contacts[0].name,    "Mom",                "contact 1 name");
    EXPECT_STR(cfg.contacts[0].room_id, "!testroom:localhost","contact 1 room_id");
    EXPECT_STR(cfg.contacts[0].avatar,  "mom",                "contact 1 avatar");
    EXPECT_STR(cfg.contacts[1].name,    "Dad",                "contact 2 name");
    EXPECT_STR(cfg.contacts[1].avatar,  "",                   "contact 2 avatar unset");

    /* Legacy contact format */
    parse_str(HDR "[contacts]\nMom = !a:x\nDad = !b:x\n", &cfg);
    EXPECT_INT(cfg.contact_count, 2, "legacy contact_count");
    EXPECT_STR(cfg.contacts[1].name,    "Dad", "legacy contact name");
    EXPECT_STR(cfg.contacts[1].room_id, "!b:x", "legacy contact room_id");

    /* sleep_* keep their lenient atoi semantics (unchanged) */
    parse_str(HDR "sleep_lock_secs = soon\n", &cfg);
    EXPECT_INT(cfg.sleep_lock_secs, 0, "sleep_lock_secs garbage -> 0 (unchanged atoi)");

    /* Required keys still required; music defaults set even on failure */
    EXPECT_INT(parse_str("access_token = tok\n", &cfg), -1, "missing homeserver_url fails");
    EXPECT_STR(cfg.music_dir, "/srv/music", "defaults set even when parse fails");
    EXPECT_INT(parse_str("homeserver_url = http://h\n", &cfg), -1, "missing access_token fails");
    EXPECT_INT(parse_str(HDR "no equals sign\n", &cfg), -1, "syntax error still fails");
}

/* contact.N.lang: the per-contact typing language */
static void test_contact_lang(void)
{
    config_t cfg;

    EXPECT_INT(parse_str(HDR "[contacts]\n"
        "contact.1.name = Mom\n"
        "contact.1.room_id = !a:x\n"
        "contact.1.lang = fr\n"
        "contact.2.name = Dad\n"
        "contact.2.room_id = !b:x\n"
        "contact.2.lang = gr\n"
        "contact.3.name = Grandma\n"
        "contact.3.room_id = !c:x\n", &cfg), 0, "lang: parse ok");
    EXPECT_INT(cfg.contacts[0].lang, LANG_FRENCH, "contact 1 lang fr");
    EXPECT_INT(cfg.contacts[1].lang, LANG_GREEK,  "contact 2 lang gr");
    EXPECT_INT(cfg.contacts[2].lang, LANG_NONE,   "contact 3 lang unset -> none");
    EXPECT(!warned("unknown"), "valid langs don't warn");

    EXPECT_INT(parse_str(HDR "[contacts]\n"
        "contact.1.name = Mom\n"
        "contact.1.lang = klingon\n", &cfg), 0, "unknown lang: parse ok");
    EXPECT_INT(cfg.contacts[0].lang, LANG_NONE, "unknown lang -> none");
    EXPECT(warned("unknown contact.1.lang 'klingon'"), "unknown lang warns");

    /* Legacy contacts have no language */
    parse_str(HDR "[contacts]\nMom = !a:x\n", &cfg);
    EXPECT_INT(cfg.contacts[0].lang, LANG_NONE, "legacy contact lang none");
}
static void test_photos(void)
{
    config_t cfg;
    EXPECT_INT(parse_str(HDR, &cfg), 0, "photos defaults: parse ok");
    EXPECT_STR(cfg.photos_dir, "/srv/photos", "default photos_dir");
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 20, "default sleep_photo_viewer_mins");

    EXPECT_INT(parse_str(HDR "photos_dir = /data/pics\nsleep_photo_viewer_mins = 45\n", &cfg),
               0, "photos keys: parse ok");
    EXPECT_STR(cfg.photos_dir, "/data/pics", "photos_dir parsed");
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 45, "sleep_photo_viewer_mins parsed");
    EXPECT(g_stderr_buf[0] == '\0', "photos keys: no warnings");

    parse_str(HDR "sleep_photo_viewer_mins = 0\n", &cfg);
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 0, "0 = no override, kept");
    EXPECT(g_stderr_buf[0] == '\0', "0: no warning");

    parse_str(HDR "sleep_photo_viewer_mins = long\n", &cfg);
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 20, "garbage keeps the default");
    EXPECT(warned("is not a number"), "garbage warns");

    parse_str(HDR "sleep_photo_viewer_mins = 20m\n", &cfg);
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 20, "a unit suffix is garbage");

    parse_str(HDR "sleep_photo_viewer_mins = -5\n", &cfg);
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 0, "negative clamps to 0");
    EXPECT(warned("clamped to 0"), "negative warns");

    parse_str(HDR "sleep_photo_viewer_mins = 99999\n", &cfg);
    EXPECT_INT(cfg.sleep_photo_viewer_mins, 1440, "huge clamps to a day");

    parse_str(HDR "photos_dir =\n", &cfg);
    EXPECT_STR(cfg.photos_dir, "/srv/photos", "empty photos_dir keeps the default");
    EXPECT(warned("photos_dir is empty"), "empty photos_dir warns");
}

static int run_self_tests(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", (t && *t) ? t : "/tmp");
    size_t len = strlen(g_tmpdir);
    if (len > 1 && g_tmpdir[len - 1] == '/')
        g_tmpdir[len - 1] = '\0';

    printf("=== config unit tests ===\n");
    test_defaults();
    test_every_key();
    test_clamping();
    test_garbage_ints();
    test_default_app_unknown();
    test_quirks();
    test_existing_keys();
    test_contact_lang();
    test_photos();

    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        return run_self_tests();

    /* CLI mode: parse a real file and dump it */
    config_t cfg;
    if (config_parse(argv[1], &cfg) != 0) {
        fprintf(stderr, "Failed to parse config file: %s\n", argv[1]);
        return 1;
    }

    config_print(&cfg);
    return 0;
}
